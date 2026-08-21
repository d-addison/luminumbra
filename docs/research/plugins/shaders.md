# Shader tooling integrations for Luminumbra

Decision-oriented research for the current runtime-GLSL renderer and the in-flight OpenGL-to-Diligent migration. The task inventory records 57 runtime-compiled GLSL files under `res/shaders/`; that count is treated as supplied context because this task's read scope does not permit a fresh asset-tree inventory.

## Current state

- `src/luminumbra_client/rendering/Shader.cpp:13-91` is the central compile/link path for vertex/fragment programs. It reads source at runtime, invokes the OpenGL driver compiler and linker, and reflects a successfully linked program.
- `Shader::ValidateLayout` stores an `ExpectedLayout` and compares it with the reflected GL layout (`Shader.cpp:97-107`). `Shader::Reload` builds a candidate program without touching the current one, reflects it, rejects a layout mismatch, and only then swaps the program and clears cached uniform locations (`Shader.cpp:112-144`). Compile, link, and layout failures therefore preserve the last known-good program.
- Spec 023 has already landed the developer-facing crawl/walk loop: F5 reload-all, one roster for all consumers, a once-per-second opt-in watcher, and an F10 ImGui panel with diagnostics and uniform probes (`docs/specs/023-live-shader-authoring/spec.md:8-34`). Tooling should wrap this workflow rather than create a second watcher or shader registry.
- The reflection foundation is broader than runtime GL. `test/CMakeLists.txt:557-590` builds the GL reflection/layout-validation test, while `test/CMakeLists.txt:593-661` finds `slangc`, compiles the two pilot HLSL fragment shaders to SPIR-V, emits Slang reflection JSON, and compares that reflection with GL introspection and `ExpectedLayout`. Slang adoption has therefore started at pilot scale; it is not merely a landscape recommendation.
- The Slang pilot is conditional. If `slangc` is absent, configure reports a skip and the test is not appended to the CTest roster (`test/CMakeLists.txt:601-662,1468-1472`). A required shader-contract lane cannot inherit that silent-absence behavior. The registration comment at `test/CMakeLists.txt:1468-1469` also still says “dxc + spirv-cross” although the target now uses Slang.
- The engine landscape ranks Slang P1 and recommends deciding at the pilot boundary, before mass-porting shaders (`docs/research/engine-library-landscape-2026.md:30-45`). That matches Diligent's model: Diligent can accept HLSL as a universal source language and backend-specific GLSL or SPIR-V representations; Slang can remain an offline producer rather than a runtime engine dependency.
- `CaptureHooks.cpp` already has the RenderDoc integration seam. It only resolves an already-injected `renderdoc.dll` (`CaptureHooks.cpp:45-89`), sets a scenario-based path template and starts capture (`CaptureHooks.cpp:204-228`), then ends the frame capture and asks RenderDoc for the resulting absolute capture path (`CaptureHooks.cpp:292-331`). If RenderDoc was not injected, it deliberately falls back to a capture-ready marker. Automation therefore needs an external launcher/injector, not another in-engine capture API.

The determinism boundary is load-bearing. Spec 023 explicitly says render-only reload and uniform edits do not feed `world_hash` (`docs/specs/023-live-shader-authoring/spec.md:36-42`). Every proposal below stays client/render-side. If shader-authored data ever changes simulation, materials, collision, world generation, or any other hashed state, it must go through the gated `SystemConfig`/additive-sub-hash/default-off/re-pin discipline instead of using these render-only paths.

## Candidate integrations

### 1. Offline GLSL validation gate, with a fast local mirror

Use `glslangValidator` as the compatibility validator for the existing OpenGL GLSL corpus. It is Khronos's reference GLSL/ESSL front end, infers stages from standard suffixes, and can validate/link stages without depending on whichever GPU driver happens to run the client. `glslc` is useful as a second, migration-oriented check because it wraps glslang and SPIRV-Tools in a compiler-like CLI, supports includes, and emits SPIR-V. It should not replace the OpenGL-semantics check: Vulkan/SPIR-V rules can reject or reinterpret code that the current GL runtime accepts.

Recommended policy:

1. Maintain one explicit shader manifest containing stage and program-pair information. Do not rely only on filename globbing: paired link validation and nonstandard names need declared intent.
2. Run `glslangValidator` over every legacy source and link each declared vertex/fragment pair. Pin the tool version in the CI image and preserve exact command lines in the artifact.
3. Promote warnings only after a clean baseline. Initially fail errors and report warnings; then adopt a reviewed warning allowlist or `-Werror` equivalent so the gate does not land as an instant 57-file cleanup project.
4. For shaders entering the RHI migration, add a `glslc`/SPIR-V compile check with an explicit target environment. This is a migration readiness signal, not proof that the raw-GL runtime will behave identically.
5. Offer the same script as an opt-in pre-commit/editor task limited to changed shaders. The Banso/CI step remains authoritative because local hooks are bypassable and developers may not have the Vulkan SDK installed.

This catches syntax, type, stage-interface, and many portability errors before runtime. It does not replace the real driver compile, GL reflection, visual parity, or rollback path; vendor extensions and driver behavior still require the runtime tests.

**Delivery form:** required Banso/CI `ShaderValidate` step backed by one version-pinned validation script and manifest; optional changed-file pre-commit/editor wrapper invoking the same script.

**Effort / risk:** **Small / Low.** The main risks are false confidence from compiling under the wrong target semantics and noisy legacy warnings. Explicit OpenGL versus Vulkan modes, a pinned version, and a staged warning policy contain both.

### 2. Incremental Slang source migration aligned with the RHI

Adopt Slang as the offline compiler and reflection authority for migrated passes, while keeping Diligent as the runtime API boundary. Slang officially supports SPIR-V and GLSL output, reflection JSON, and modular shader organization. Diligent officially supports HLSL as a universal source language as well as backend-specific shader representations. The clean ownership split is:

`Slang/HLSL source -> pinned slangc -> backend artifact + reflection JSON -> Diligent shader/pipeline creation`

Do not mass-convert all legacy GLSL before the RHI owns those passes. The safer sequence is:

1. Freeze the pilot's compiler version, flags, reflection JSON schema expectations, entry-point convention, coordinate conventions, matrix layout, and binding policy.
2. Make the pilot gate non-optional on a dedicated toolchain lane and correct the stale dxc/spirv-cross registration comment when implementation scope permits.
3. Migrate one complete pass family at a time. During coexistence, raw GL keeps its current GLSL source and rollback-safe reload path; the Diligent implementation consumes the Slang-produced artifact. `ExpectedLayout` and a visual/parity fixture are the bridge between the two implementations.
4. Once a pass is owned by Diligent on all supported backends, make its Slang/HLSL file the source of truth. Emit SPIR-V for Vulkan, DXIL when the DX12 backend arrives, and either HLSL/GLSL or the representation supported by the selected Diligent GL route. Do not maintain hand-edited GLSL and Slang indefinitely after the parity window closes.
5. Introduce Slang modules only after several passes reveal stable shared boundaries. Lighting, atmosphere, and noise are plausible modules, but premature module extraction would couple unrelated migration diffs.

This sequencing avoids two incompatible big-bang changes: shader-language conversion and RHI ownership. It also prevents the live-authoring loop from regressing. A migrated pass must retain a developer compile/reload path; if offline compilation is too slow for the one-second watcher, use an asynchronous developer compile whose artifact is adopted only after compile plus reflection validation succeeds, preserving the current last-known-good contract.

**Delivery form:** pinned `slangc` CMake build target and artifact convention, a per-pass migration playbook/checklist, and an expanding CTest parity matrix rooted in the existing two-shader pilot.

**Effort / risk:** **Large / Medium-High.** Compiler hookup is already proven and low-risk; mass source conversion is not. Binding allocation, matrix packing, clip-space conventions, dead-resource optimization, generated-GLSL readability, compile latency, and dual-path drift are the material risks. Coupling each conversion to its Diligent pass and requiring reflection plus image/parity evidence keeps the risk bounded.

### 3. Live-authoring workflow wrapper around spec 023

The engine already owns watching, rollback, diagnostics, and shader enumeration. The missing integration is a consistent way to enter and observe that loop. Add a developer-only launch recipe that starts the correct build/configuration, opens or advertises the F10 shader panel, and runs the same changed-file offline validator used by candidate 1 before the engine sees the edit. Editor tasks may invoke validation on save, but must not write generated content into `res/shaders/` or create a parallel filesystem watcher.

Normalize diagnostics into `path:line:column: severity: message` where the external compiler provides positions, then preserve the raw driver/Slang text underneath. The panel remains the authority for whether a candidate was actually adopted. A useful session artifact is a small append-only record containing source path, timestamp, validation result, reload result, and rollback diagnostic; it should contain no simulation state and should be opt-in/dev-only.

For Slang-migrated passes, the wrapper may run `slangc` into a build or user cache and notify the existing main-thread reload mechanism only after the artifact and reflection checks pass. It must retain the roster from spec 023 as the live set. Any new material or pass drop-in feature remains outside this candidate and, if it gains simulation properties, is subject to the `world_hash` contract.

**Delivery form:** developer launch preset plus VS Code/editor tasks and a thin `shader-watch` workflow command that delegates to the existing spec-023 watcher/panel; optional session-log artifact.

**Effort / risk:** **Small-Medium / Low.** The chief risk is duplicate ownership between editor, wrapper, and engine. Keeping the engine roster/watcher authoritative and treating external validation as advisory before reload avoids races and behavior drift.

### 4. ExpectedLayout and reflection contract gates in CI

Promote the existing pilot pattern into the central shader interface gate. For every migrated shader/pass, CI should compare three deliberately independent declarations:

- authored `ExpectedLayout` for what the pass promises to bind;
- Slang reflection JSON for what the offline compiler emitted;
- GL program reflection for the legacy/raw-GL parity implementation while that path exists.

The gate should validate resource names, kinds, stages, array counts, binding/set locations where contractual, block sizes/member offsets where used, and required versus optional resources. Normalize each source into one canonical report before comparing so backend-specific JSON ordering and incidental names do not create churn. Optimization may legally remove inactive resources, so the contract needs an explicit optional/inactive policy rather than weakening every mismatch to a warning.

Two lanes are appropriate:

- A fast offline lane compiles and reflects every migrated shader with pinned `slangc`; absence of the compiler is a configuration failure, not a skipped green test.
- A GPU/GL lane links representative or all still-supported GL pairs in a hidden context and compares driver reflection. Keep this lane on a controlled GPU/driver image because active-resource behavior can vary.

Emit a machine-readable artifact per shader with compiler version, source digest, entry point, normalized reflected layout, expected layout, and verdict. This makes layout drift reviewable and lets Banso surface the exact resource mismatch. Continue testing `Shader::Reload` rollback separately: a green offline contract does not prove the runtime candidate-swap path.

**Delivery form:** required CTest/Banso `ShaderLayoutContract` gate plus normalized JSON artifacts, expanding the current `shader_reflection_test` and `pilot_shader_reflection_test` pattern.

**Effort / risk:** **Medium / Low-Medium.** Parser/schema normalization and classifying optional resources are the work. False failures from inactive resources and platform reflection differences are the primary risk; a canonical schema and controlled GL lane address them. This is render-only and does not affect `world_hash`.

### 5. RenderDoc scenario capture automation through CaptureHooks

Use RenderDoc's launcher to inject the DLL and let the existing engine code delimit the frame. The runner should launch a deterministic named render scenario under `renderdoccmd capture --wait-for-exit`, pass a unique artifact directory and scenario name to the client, wait for the capture-ready marker, and require `EndFrameCapture` to report success plus a real `.rdc` path. This matches `CaptureHooks.cpp`: the engine intentionally does not load RenderDoc itself, and the in-application API already controls the path template and retrieves the completed capture path.

Start with artifact production, not pixel baselining. The first gate should verify:

- the requested scenario reached the capture bracket;
- the backend is RenderDoc rather than marker-only fallback;
- exactly the expected bounded number of captures was produced;
- the returned file exists, is non-empty, and is retained with logs and build/compiler metadata;
- the scenario exits successfully and, if it advances simulation, its normal run/replay `world_hash` evidence remains unchanged.

Add replay inspection only in a pinned RenderDoc environment. RenderDoc's Python replay API can open captures and expose frame analysis, but its standalone Python module has version/distribution constraints. A later nightly check can inspect that a frame replays, contains expected top-level actions/markers, has no API validation errors, and exposes the expected pipeline resources. Avoid making portable PR validation depend on replaying `.rdc` files across arbitrary drivers.

Run this as manual or nightly GPU evidence first. Captures are driver/backend-sensitive, relatively large, and can expose nondeterministic presentation details even when simulation is deterministic. When a Diligent Vulkan path becomes stable, run the same named scenario and capture contract on both GL and Vulkan; do not compare opaque `.rdc` bytes.

**Delivery form:** PowerShell/CI capture runner using `renderdoccmd`, existing `CaptureHooks`, and retained `.rdc` artifacts; optional pinned-version Python replay smoke test in a nightly GPU lane.

**Effort / risk:** **Medium / Medium-High.** Injection and capture bracketing are already implemented. Reliable GPU runners, RenderDoc version pinning, timeouts, artifact size, replay/driver compatibility, and clean failure reporting are the remaining risks. Keeping it manual/nightly until stable prevents a flaky PR gate.

## Ranking

| Rank | Candidate | Recommended timing | Delivery form | Effort | Risk | Decision |
|---:|---|---|---|---|---|---|
| 1 | Offline GLSL validation | Now | Required Banso/CI step; optional pre-commit/editor mirror | Small | Low | Adopt first; fastest coverage for all legacy shaders |
| 2 | ExpectedLayout/reflection contracts | Now, expanding with each migrated pass | CTest/Banso gate plus normalized JSON | Medium | Low-Medium | Adopt; make the current pilot non-optional in its toolchain lane |
| 3 | Slang migration aligned with Diligent | Now at the pilot boundary, then per pass | Pinned build target, playbook, parity matrix | Large | Medium-High | Proceed incrementally; do not mass-port independently of the RHI |
| 4 | Live-authoring workflow wrapper | After the validator command is reusable | Dev preset/editor tasks delegating to spec 023 | Small-Medium | Low | Adopt as developer ergonomics; no second watcher/roster |
| 5 | RenderDoc capture automation | Manual/nightly now; gate only after runner stability | `renderdoccmd` runner plus `.rdc` artifact, later replay smoke | Medium | Medium-High | Pilot on a controlled GPU runner |

The first two items form the immediate quality floor. The third is the architectural path and should consume those gates as it advances. The fourth improves iteration without changing ownership. The fifth supplies high-value forensic evidence but belongs on controlled hardware until its failure rate and artifact cost are known.

## Sources consulted

Repository evidence (no `README.md` architecture claims were used):

- `src/luminumbra_client/rendering/Shader.cpp`, especially lines 13-91 and 97-144.
- `src/luminumbra_client/rendering/CaptureHooks.cpp`, especially lines 45-89, 204-228, and 292-331.
- `docs/specs/023-live-shader-authoring/spec.md`, especially lines 8-42.
- `docs/research/engine-library-landscape-2026.md`, especially lines 30-45 and 84.
- `test/CMakeLists.txt`, especially lines 557-661 and 1468-1472.
- Task brief inventory for the 57 GLSL files and the `res/shaders/pilot/` scope.

Primary external sources:

- Khronos glslang repository and validator overview: <https://github.com/KhronosGroup/glslang>
- Google shaderc/glslc overview: <https://github.com/google/shaderc>
- Slang command-line reference, including `-reflection-json` and target options: <https://docs.shader-slang.org/en/stable/external/slang/docs/command-line-slangc-reference.html>
- Slang getting-started guide for SPIR-V and GLSL targets: <https://docs.shader-slang.org/en/stable/external/slang/docs/user-guide/01-get-started.html>
- Diligent shader creation and supported source-language model: <https://diligentgraphics.com/diligent-engine/using-the-api/>
- RenderDoc in-application API: <https://github.com/baldurk/renderdoc/blob/v1.x/docs/in_application_api.rst>
- RenderDoc application API header (`SetCaptureFilePathTemplate`, `GetCapture`, and frame capture API): <https://github.com/baldurk/renderdoc/blob/v1.x/renderdoc/api/app/renderdoc_app.h>
- RenderDoc command-line capture implementation: <https://github.com/baldurk/renderdoc/blob/v1.x/renderdoccmd/renderdoccmd.cpp>
- RenderDoc Python API overview and distribution constraints: <https://github.com/baldurk/renderdoc/blob/v1.x/docs/python_api/index.rst>
