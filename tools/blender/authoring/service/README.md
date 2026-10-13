# Installed asset build service

`luminumbra-author` runs an explicitly configured native `asset_processor` in a
separate process. It snapshots source files, validates compiled assets,
and publishes a generation only after the build succeeds. This optional Python
3.11+ tool has no Blender or game dependency and uses only the standard library.

The `glb-geometry-v1` profile produces flattened LMSH geometry or one-skin LMS2
geometry with LANM clips, according to the installed compiler's qualified profile.
The optional [static prefab profile](PREFABS.md) preserves hierarchy and instances
with material bindings and compiled textures. The optional
[compiled prefab runtime](../../../prefab_runtime/README.md) provides ECS
instantiation and an installed headless inspector for retained generations.
The service itself does not instantiate prefabs. Components, graphs, rendering
and engine preview frames remain unavailable; `capabilities` describes the
service's operations separately from the optional runtime consumer.
Compiler fidelity must be qualified with the exact installed executable: pinning
a hash does not establish that an older compiler supports a new asset feature.

## Package and configure

Build an executable Python archive into an isolated installation:

```sh
python tools/blender/authoring/service/package.py /your/install/luminumbra-author
```

Use `python /your/install/luminumbra-author` on Windows. The archive runs without
the source checkout. Install the native asset compiler and its required engine
libraries separately; this package never rebuilds them or downloads tools.

Place a toolchain manifest beside the installed compiler. Paths are relative to
the manifest, and every declared file needs its actual lowercase SHA-256 digest:

```json
{
  "schema": "luminumbra.authoring.toolchain.v1",
  "id": "project.qualified_compiler",
  "processor": "asset_processor",
  "files": {"asset_processor": "<actual SHA-256>"}
}
```

Use `asset_processor.exe` on Windows and include the required installed libraries
in `files`. The caller chooses this trusted configuration at service launch;
build requests cannot supply a command, script or arbitrary compiler arguments.
The service hashes these members before and after compilation. Qualify the host's
system runtime separately. Different host/compiler profiles need separate evidence.

## Author and build a snapshot

An asset sidecar selects the geometry profile and records its exporter identity:

```json
{
  "schema": "luminumbra.authoring.asset.v1",
  "asset_id": "project.prop",
  "revision": 1,
  "profile": "glb-geometry-v1",
  "source": "exports/prop.glb",
  "dependencies": ["source/prop.blend"],
  "exporter": {"id": "blender.gltf.qualified", "sha256": "<actual exporter tree SHA-256>"},
  "settings": {"emit_lods": false}
}
```

All files must already exist inside the project, without symlinks or path aliases.
The GLB must embed its buffers and images. `.blend` dependencies contribute bytes
and hashes only; the service never opens them in Blender or executes their scripts.
The exporter identity is a declaration from the snapshot producer, recorded in
build identity. Qualification of that exporter remains a separate fixture lane.

Snapshot producers may supply `expected_dependency_hashes`, an object mapping
declared dependency paths to their captured SHA-256 digests. Extraction refuses
a mismatch before running the compiler. The Blender adapter uses this guard to
prevent an edit made before service extraction from publishing an older export.
Dependencies are checked again before publication. These producer guards are
optional for ordinary immutable CLI inputs.

`required_schemas` may contain only `luminumbra.authoring.asset.v1`. Unknown fields,
required extensions and engine metadata are refused. The only engine extras
accepted in GLB are string correlation IDs named `luminumbra.object_id`,
`luminumbra.asset_id` and `luminumbra.material_id`. Optional sidecar `annotations`
are inert JSON objects. This profile does not interpret components or behaviors.

```sh
python /your/install/luminumbra-author --project /your/project --toolchain /your/install/toolchain.json validate asset.json
python /your/install/luminumbra-author --project /your/project --toolchain /your/install/toolchain.json build asset.json --revision 1
python /your/install/luminumbra-author --project /your/project --toolchain /your/install/toolchain.json generation.inspect
```

`validate` checks the sidecar and GLB containment. `build` waits for native geometry
validation, compilation, output readback and publication; it exits nonzero on
failure, cancellation or a stale revision. Diagnostics contain rule IDs and field
locations. Native compiler diagnostics appear in a bounded receipt log. Ctrl-C
cancels a standalone build. Resource counters report compiled bytes, vertices,
triangles, joints and clip tracks, with no GPU-memory or rendering claims.

## Session RPC and lifecycle

Start `serve` with `LUMINUMBRA_AUTHOR_TOKEN` set to an unpredictable token of at
least 32 characters. Its inherited stdin/stdout pipes carry newline-delimited
JSON. There is no socket or network listener. Every request includes the token:

```json
{"id":1,"token":"<session token>","op":"build","params":{"source":"asset.json","revision":1}}
```

Responses echo the integer `id` and contain either `ok: true, result: ...` or
`ok: false, findings: [...]`. Supported operations are `capabilities`, `registry`,
`validate`, `build`, `job.status`, `job.cancel` and `generation.inspect`.
Build submission returns a job ID; compilation advances independently of polling.
Status/cancel take `job_id`; generation inspection takes an optional job ID and
otherwise returns the current generation. Cancellation becomes terminal after
the owned worker stops. A request arriving after publication observes success.
Use these RPC operations within the same session to inspect or cancel live jobs;
independent CLI invocations cannot take over another session's project lock.

One service owns a project per host. Linux uses `flock`; Windows uses a kernel
mutex. These locks do not coordinate simultaneous Windows/Linux access or different
path aliases. Use a canonical project path and trusted local collaborators.
Source writers should publish sidecars atomically and advance revisions; arbitrary
concurrent hostile filesystem mutation is outside this profile.

The compiler supervisor holds an inherited ownership pipe. Closing the session,
cancelling a job or losing the service process closes that pipe and stops the
compiler. The default compiler timeout is 120 seconds, configurable up to one
hour. The registered compiler is a single native process; arbitrary process trees
are not a supported toolchain on Windows. Logs are drained and retain at most
64 KiB; requests are at most 64 KiB, snapshots and output sets at most 256 MiB,
and the project retains at most 256 jobs. Size limits are checked during
extraction and output validation, not an operating-system disk quota.

## Publication and recovery

Private inputs, outputs and receipts live under `.luminumbra-author/jobs/`.
Output readback checks exact layouts/counts, finite attributes, mesh indices,
joint order, influence budgets and clip target/interpolation contracts. A completed
generation moves into `.luminumbra-author/generations/`. The service rechecks the
source hashes before atomically replacing `current.json`; failed, cancelled and
stale work leaves the previous pointer intact.

Consumers retain a generation ID and read its manifest instead of following the
current pointer on every access. Published generations are never modified or
automatically deleted. Garbage collection and power-loss durability are future
work; an atomic pointer does not imply a durable directory fsync on every host.
Startup marks interrupted jobs failed, except when their generation pointer
already committed. That case recovers as success after hash verification.

Receipts record the host/Python profile, source/dependency hashes, exporter metadata through the hashed
sidecar, service-code hashes, compiler/library identities, settings, outputs and
timing. This timing ends at publication, not at a visible Blender viewport frame.
The receipt and generation schemas remain tooling contracts; no saved-world or
network state format changes are introduced by this service.

## Meshgen configuration and game script pins

The source invocation of the read-only configuration check uses the trusted
launcher with isolated, no-site, no-bytecode Python startup. Supply an absolute
path to the reviewed service launcher, outside the game/project:

```sh
python3 -I -S -B /your/engine/tools/blender/authoring/service/launch.py --project /your/game --toolchain /your/install/toolchain.json meshgen check-config --profile rock --print-effective
```

In PowerShell, use `python -I -S -B C:\your\engine\tools\blender\authoring\service\launch.py`
with the same arguments and absolute Windows paths. `-I` ignores `PYTHONPATH`
and `PYTHONDONTWRITEBYTECODE`, so `-B` is required even if that environment
variable is set. `-S` prevents startup `.pth`, `sitecustomize` and `usercustomize`
processing. Before importing the service, the launcher explicitly sets
`sys.path` to the service package root and interpreter stdlib locations only;
cwd and the game/project root are excluded. For the installed zip application,
use `python -I -S -B /your/install/luminumbra-author.pyz` with the same check-config
arguments; the archive is its package root. The supported launch creates no bytecode, state,
session lock, backend connection, secret-value read or Blender process. The
required toolchain argument retains the existing CLI shape; this planning check
does not verify executables. Live backend validation is reported as unavailable.

Toolchain manifests may include `scripts`, a role mapping. Each role declares a
project-relative `.py` entry `path`, its `sha256`, and a `modules` mapping from
companion `.py` paths (including regular package initializers) to SHA-256 values.
Requests cannot choose scripts. Old compiler-only toolchains remain valid;
MG2 script use requires pins, and clients must negotiate `meshgen_script_pins`.

`Toolchain.verify_scripts()` rechecks all pins and provides conservative AST
diagnostics. Its import path is the entry directory followed by companion roots
derived from the pinned files, above each outermost pinned regular package.
Optional `import_roots` lists additional canonical project-relative directories
containing pinned companions (`"."` means the project root); namespace packages
may need this explicit root. Every transitive import uses that same path.
Symlinks, junctions and project shadows of stdlib or Blender module names are
refused, even when the shadow has a pin. Assignment aliases of `__import__`,
importlib spec/loader APIs and `exec`/`eval`/`compile` are diagnosed as unsupported.
AST diagnostics alone do not establish a complete inventory.

Execution must use the trusted launcher in a fresh interpreter:

```sh
python3 -I -S -B /your/engine/tools/blender/authoring/service/launch.py --scripts --project /your/game --toolchain /your/install/toolchain.json --role finisher
```

Hosts calling `meshgen_scripts.run_pinned_script()` through another entry point
must establish the same startup contract before importing the service. The
guarded path returns a structured `toolchain.bootstrap` refusal unless Python
has isolated/no-site/no-bytecode flags and only service/stdlib paths on `sys.path`,
with no project path. Removing unsafe paths after ambient startup is too late.

The launcher installs an import finder and a permanent audit hook before adding
the controlled companion import roots. Each project module
is read through containment checks, hashed, and compiled from those exact bytes;
cached bytecode is never consumed. Project module origins and search locations
must use a source loader and a pinned source file with a matching digest.
Sourceless, native and other non-source project loaders are refused regardless
of suffix, suffix case or embedded code filename. Directly obtained source
loaders use the same verified snapshots. Unpinned or changed files are refused
at load time. Project `.pth` reads and `site.addsitedir`/`site.addpackage` calls
targeting the project or its descendants are refused before processing.
Execution provenance walks the whole frame stack, so stdlib helpers cannot
launder unpinned source or synthetic-filename `exec` from project callers.
Actual stdlib module imports and their initialization remain supported. Code
already imported from the project is refused before launch. Interpreter and
engine installations remain trusted; native Blender integration is E4 work.

This guard enforces source integrity for trusted, reviewed scripts. It is not an
OS sandbox against malicious pinned Python that changes interpreter internals,
uses native code or deliberately delegates arbitrary execution to trusted
external libraries. The E4 supervisor must use this launcher contract together
with its process/resource envelope; E1 does not wire up or qualify Blender
execution. Provisional profile budgets remain uncalibrated.

## Verification

```sh
python -B -m unittest discover -s tools/blender/authoring/service/tests -v
python tools/blender/authoring/service/tests/native_acceptance.py --service /your/install/luminumbra-author --toolchain /your/install/toolchain.json --output /fresh/evidence/directory --exporter-id blender.gltf.qualified --exporter-sha256 ACTUAL_EXPORTER_SHA256 prop.glb plant.glb character.glb
```

Unit tests inject compiler failures and publication races. The separate native
runner requires a real installed compiler and caller-provided GLBs, builds each
twice, compares output hashes, retains old generations, exercises refusals and
checks unchanged toolchain hashes. Neither lane closes renderer, physical
animation or newly authored production-content acceptance.
