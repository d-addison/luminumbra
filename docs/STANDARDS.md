# Luminumbra Engineering Standards

**Status:** authoritative. Established 2026-06-18. This is the single entry point for "how we
build luminumbra." It *consolidates and codifies* standards that were previously de-facto or
scattered; where a topic has a deeper dedicated doc, this file states the rule and links out.

## 0. Precedence

When guidance conflicts, the higher item wins:

1. A formal spec under `.forge/specs/<feature>/spec.md` (the SDD source of truth for that feature).
2. The locked invariants in `test/features/TDD-LOCK.md` and per-iteration
   `.forge/artifacts/engine-iteration-*/design-decisions.md`.
3. **This document.**
4. Dedicated docs: `docs/TDD.md`, `docs/build/dependency-policy.md`, `CONTRIBUTING.md`, `README.md`.
5. De-facto code patterns.

If you must deviate, say so in the commit/PR and (for a binding rule) update this doc in the same change.

---

## 1. Language & code style

- **C++20**, extensions OFF (`CMakeLists.txt`). No C++23 features.
- **Formatting is mechanical:** `.clang-format` (LLVM base, 100-col) is the law. Run it; never
  hand-format against it. `.clang-tidy` checks (bugprone/clang-analyzer/modern/performance/
  portability) must stay clean.
- **Headers:** `#pragma once` (never include guards). Include order: own header first, then C++
  std, then third-party, then project headers — separated by blank lines, each group sorted.
- **Warnings are errors.** Builds run `-Werror` / `/WX` (preset `*-WARNINGS_AS_ERRORS=ON`). An
  unused function/param/variable fails the build — delete dead code, don't suppress.
- **Numeric types:** use the aliases in `include/luminumbra/core/Types.h` (`u8…u64`, `i8…i64`,
  `f32/f64`) for sized types; plain `int`/`float` for local arithmetic is fine.
- **`[[nodiscard]]`** on every query/getter whose result has no side effect.
- **Error handling:** prefer total functions — `std::optional<T>` for "maybe", a result struct
  (`{bool ok; std::vector<std::string> errors;}`, see `core/EngineContracts.h`) for validation.
  `LUMINUMBRA_ASSERT` (`core/Debug.h`) for programmer-error invariants. Exceptions are used only
  at coarse boundaries (e.g. JSON parse) and **must not** escape onto the sim tick path.
- **Naming:** types/components `PascalCase`, `*Component` / `*System` suffixes; functions and
  variables `lower_snake_case` or `camelCase` matching the surrounding file (don't mix within a
  file); compile-time constants `kPascalCase`.

---

## 2. Namespace policy  *(decision 2026-06-18)*

**The standard for all NEW code is lowercase `luminumbra::` with `snake_case` sub-namespaces**
(`luminumbra::core`, `luminumbra::ai`, `luminumbra::foliage`, …). This matches the C++ standard
library / Core Guidelines convention, mirrors the directory layout, and is what the newer engine
code already uses.

- The older **PascalCase `Luminumbra::…`** namespaces (`Luminumbra::Components`,
  `::Persistence`, `::Net`, `::Ecs`, `::Systems`, …) are **legacy**. Do **not** do a big-bang
  rename — migrate a unit to lowercase **opportunistically, only when you're already editing it**,
  with a determinism + (if rendering) visual diff proving no behavior change (same discipline as
  the SystemConfig §1b lazy migration).
- Until a unit is migrated, **match its existing namespace** — never introduce a third style.
- Crossing the boundary: alias at the top of a lowercase unit, e.g.
  `namespace comp = ::Luminumbra::Components;` — do not sprinkle fully-qualified names.

---

## 3. Architecture & engine/game decoupling

Three modules, one direction of dependency (`server`/`client` → `common`; never the reverse):

- **`luminumbra_common`** — engine: ECS, simulation, systems, networking, persistence.
  **Generic.** Knows nothing about cameras, specific creatures, or the Project Capture game.
- **`luminumbra_client`** — presentation: rendering, audio, UI, input, the player camera.
- **`luminumbra_server`** — host authority, admin, tuning.

**The rule:** engine code stays generic; **game content is data** (`data/common/*.json`,
`scripts/`, archetypes, biomes, materials, `systems.json`). If a `common/` change references a
LuminCrystal/Aetheric/Project-Capture concept by name, it's in the wrong layer. The
`EngineGameSplitLint` and `SimDeterminismLint` engine-frontier gates enforce this; keep them green.

---

## 4. Determinism contract  *(binding — the engine's spine)*

The simulation is **float-but-deterministic** so `run == replay` holds bit-for-bit across machines.
Full rules: `core/DeterministicMath.h`, `test/features/TDD-LOCK.md`, iteration design-decisions.
On any code that runs on the sim tick path:

- **Seeded RNG only** — `luminumbra::core::DeterministicRng`; no global/wall-clock RNG.
- **No libm transcendentals** — use `DeterministicMath::{sin,cos,atan2,sqrt,…}`.
- **No wall-clock**, no `std::chrono` reads, no unordered-container iteration order dependence.
- **`-ffp-contract=off`** is pinned on `luminumbra_common`/server — never re-enable FMA contraction.
- **Id-ordered traversal** — sort by entity id (or another stable key) before any order-sensitive
  reduction (boids neighbour sums, etc.).
- **Seed-offset registry is append-only** (wind+11, weather+12/13, aether+14, plant+15, …): a new
  sim system claims the next free offset and records it in design-decisions. Collisions are defects.
- **Tick order is canonical** (`GameSession::TickSimulation`); new sim systems append at the end,
  gated by a participant/flag check so the canonical (empty-participant) roster stays byte-identical.

### World hash & sub-hashes
- The top-level `world_hash` (`ComputeWorldStreamingStateHash`, canonical baseline
  `d950a6afc12a5cdc`) is computed from world/streaming **state**.
- Per-system sub-hashes (`wind`/`weather`/`aether`/`scent`/`plant`/`config`, in
  `persistence/WorldPersistenceRoundtrip.h`) are **ADDITIVE and SEPARATE** — they never alter the
  top-level hash, and each returns **empty `{}` when its system has no participants / is at default**.
- A `world_hash` movement must be **deliberate, single-step, and re-blessed** (heavy oracle + LREC1
  replay + lockstep evidence) per `AC-I6-TDD-LOCK-004`. During local dev, intentional bumps are fine
  (re-pin the baseline literal, keep `run==replay`) — but adding a *flag/registry* must **not** move it.
- Replay is `LREC1`, **tick-indexed** (never frame-indexed); recorded inputs from a recorded boot
  must reproduce identical hashes at checkpoints. Lockstep latency/horizon machinery lives **outside**
  the hash (measured in ticks, never RTT/wall-clock).

---

## 5. Configuration & settings  *(SystemConfig is the single registry — decision 2026-06-18)*

All configuration — engine feature-flags, per-system tuning, **and** player-facing settings — lives
in **one** registry: `luminumbra::core::SystemConfig` (`core/SystemConfig.{h,cpp}`, spec
`.forge/specs/system-config/spec.md`). There is no second config system.

**Three sections, one determinism boundary:**

| Section   | May affect sim state? | Hashed? | Persisted where |
|-----------|----------------------|---------|-----------------|
| `sim.*`   | yes                  | **yes** — via the additive `ComputeConfigSubHash()` (empty at defaults) | `data/common/systems.json` (tracked, dev/game data) |
| `render.*`| no (render-only)     | **never** | `data/common/systems.json` |
| `user.*`  | no (client-only player settings: video/audio/controls) | **never** | per-user overlay: `%APPDATA%/Luminumbra/settings.json` (writable) |

**Standing rules:**
- **Default-OFF discipline.** Every new system flag defaults to `enabled=false` until its gate is
  green, so the all-off baseline and `world_hash` stay byte-identical. New sim systems read
  `SystemConfig::enabled(key)` in init/tick and no-op when off.
- **Two gated baselines** (critique Objection 5): the all-off `systems.json` *and* the intended-ON
  **game profile** `systems.game.json` are both kept green every slice. As a pillar lands, flip its
  flag ON in the game profile and re-bless that baseline.
- **`enabled()` is O(1)** (packed-bitset bit test on a resolved immutable snapshot) — safe to call
  per-entity on the 36k-entity / 30 Hz tick. No map lookup / string compare / allocation on hot paths.
- **`user.*` never touches any hash** — video/audio/controls are client-only and live in the
  writable per-user overlay; the overlay only ever contains `user.*`. Load order = `systems.json`
  defaults, then overlay the per-user file (then env/CLI for dev override).
- Adding a flag/param/section is **not** a `world_hash` bump (it's additive and empty-at-default).

---

## 6. Input & controls

- **Action-map, not raw keys.** Gameplay reads logical `InputAction`s (Move_Forward, Jump, Sprint,
  Crouch, ToggleNoclip, Look_*, …), never `GLFW_KEY_*` directly. Bindings live in `user.controls.*`
  (action → key code), so controls are rebindable and persisted with the rest of user settings (§5).
- **`PlayerController` consumes resolved actions** (extend `PlayerReplayInputFrame`), not GLFW polls
  scattered across `main_client.cpp`. Mouse **sensitivity** and **FOV** are `user.*` settings, not
  compile-time constants.
- **Sim vs client:** single-player movement may be client-side, but the input path must be shaped so
  it can produce the deterministic `UsercmdMsg` (`net/ReplicationProtocol.h`) for lockstep — input
  that feeds the sim obeys §4.

---

## 7. Testing & gates  *(the bar for "done")*

Per `test/features/TDD-LOCK.md` and `docs/TDD.md`:

- **Test-first (TDD).** Every acceptance criterion gets a failing test/gate **before** production
  code. Watch it fail (it must bite), then implement the minimum to green.
- **SDD trace.** Each AC links to exactly one proving signal: a `ctest`, an engine-frontier gate
  (`.forge/scripts/validate-engine-frontier.ps1 -Mode <X>`), or a BDD scenario.
- **Determinism test is mandatory for every sim system** (`run==replay` + a `Compute<X>SubHash`).
- **Visual debt closes only on a passing `WorldVisualSweep`** (`tools/visual_critique.py … --strict`,
  0/48). Detector false-positives get a **fixture-backed** fix in `tools/test_visual_critique.py`,
  never a reclassification. Re-bless only when the look changed intentionally.
- **"Done"** = the AC's test is green AND the relevant gates/baselines (incl. both config baselines)
  hold. Not before.
- New gtests register in `test/CMakeLists.txt`; dependency-light unit tests join `common_tests`.

---

## 8. Build & toolchain

- **One canonical tree — always build through a preset:** `cmake --build --preset debug` →
  `build/debug` (the tree the engine-frontier gates read); `--preset release` → `build/release`, etc.
  Do **not** configure the legacy root `build/` tree (`cmake -B build`) — a second configured tree
  lets a gate build one tree and read another (stale code). The `validate-build-tree.ps1 -Strict`
  preflight (the `BuildTreeStrict` gate) hard-fails on a concurrent root `build/CMakeCache.txt`.
- **PATH hygiene (Windows):** prepend `C:\msys64\ucrt64\bin` to PATH for every build/ctest/validator
  call — KiCad/mingw64 on PATH ahead of ucrt64 causes silent `cc1plus` failures.
- **Presets:** `debug`, `debug-asan`, `release` (`CMakePresets.json`), warnings-as-errors ON.
- **Source manifests:** every new `.cpp` is added to the module's `sources.cmake` in the same change.
- **Dependencies stay vendored** (`docs/build/dependency-policy.md`) — no vcpkg/Conan; local CMake
  targets; vendor tests/examples/benchmarks disabled. (GNS/Steam transports are opt-in exceptions.)

---

## 9. Commit & workflow

- **This branch (`feat/polyglot-audit-roadmap`) is LOCAL-ONLY — never push.**
- **Forge SDD+TDD lifecycle** drives feature work: brainstorm → spec → grill/critique/pre-mortem →
  write-failing-tests → execute → verify. **Tier the ceremony** to risk: T1 (full lifecycle) for
  cross-cutting/risky systems; T2 (spec + tests + verify) for self-contained systems with clear ACs;
  T3 (test + implement) for mechanical toggles. Determinism tests stay mandatory for all sim items.
- **Commit per slice; split unrelated concerns.** Conventional-commit subject
  (`feat(scope): …`, `fix(scope): …`). End messages with the `Co-Authored-By:` trailer.
- **Rollback rule:** a slice that breaks a baseline is **reverted by default**; re-pin a baseline
  literal only with a written justification of why the behavior change is intended.

---

## 10. Contributor checklist (pre-commit)

- [ ] Spec exists / updated; every requirement has a testable AC.
- [ ] Failing test written first, from the AC; now green.
- [ ] Sim change? determinism test + `Compute<X>SubHash` added; default-OFF; seed offset recorded.
- [ ] `world_hash` unchanged (or one deliberate, re-blessed, justified bump).
- [ ] New system reads `SystemConfig::enabled(...)`; flag added to `systems.json` (+ game profile if ON).
- [ ] Player setting? it's in `user.*`, never hashed, persisted to the per-user overlay.
- [ ] `.cpp` added to `sources.cmake`; built in the **tree you tested** with msys64 PATH; `-Werror` clean.
- [ ] Visual-touching? `WorldVisualSweep --strict` green (re-blessed only if intended).
- [ ] Namespace = lowercase `luminumbra::` for new code (or matches the file being edited).
- [ ] Committed per slice, unrelated concerns split, `Co-Authored-By` trailer; **not pushed**.
