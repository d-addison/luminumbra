# Determinism residency contract (two worlds + availability set)

**Status:** CONTRACT (created 2026-06-27, Spec 018 track 018-A1). Behavior-neutral.
**Header:** `src/luminumbra_common/core/ResidencyContract.h`
**Spec:** `docs/specs/018-determinism-hardening/spec.md` (FR-A + FR-B)
**Consumed by:** Spec 017-B (engine concurrency) — the deterministic availability set.

> This document and its header add **zero engine behavior**. They formalize a partition
> the engine already enforces informally. The canonical baseline
> `luminumbra_server_app --smoke == 6f008a9f637c40b7`, run==replay, must hold unchanged —
> the header is included by no translation unit yet, so it cannot move the hash.

---

## 1. Why this contract exists

The run/replay + `world_hash` system is a strong *discipline* but not yet a *framework*:
the two-worlds split is real but encoded as "remember to leave this out of the match."
Concretely, today:

- The **render mesh** sub-hash is computed and reported but deliberately **omitted** from
  the run==replay `sub_hashes_match` set (`src/luminumbra_server/main_server.cpp:469-478`)
  and never folded into the composite (`src/luminumbra_server/ServerWorldRunner.cpp:527-533`),
  "because collision uses the heightmap, not this mesh."
- **`SystemConfig`** hashes only `Section::Sim` keys and skips `render.*`
  (`src/luminumbra_common/core/SystemConfig.cpp:223`), emitting the empty baseline when
  every sim flag is at its compiled default (`:243`).

Both are the *right idea* — a deterministic SIMULATION world and a nondeterministic RENDER
world — expressed as a hand-maintained exclusion. This contract names the partition so the
illegal state (a render value feeding the hash, or a sim value silently dropped) becomes a
declared, checkable fact rather than a thing engineers must remember.

---

## 2. The two residency classes (FR-A-001)

`ResidencyContract.h` declares both, in one authoritative location:

| Class | Determinism | May feed `world_hash`? | Examples |
|-------|-------------|------------------------|----------|
| `SimResidency`    | deterministic     | **yes** | terrain, water depth, entities, wind, weather, aether, scents, ecology, plants |
| `RenderResidency` | nondeterministic  | **no**  | render mesh (worker-order vertex/index order), GPU readbacks, exposure metering, froxel temporal jitter |

The single rule is the compile-time predicate:

```cpp
constexpr bool MayFeedWorldHash(ResidencyClass c) noexcept { return c == ResidencyClass::Sim; }
```

The header self-checks it:

```cpp
static_assert( MayFeedWorldHash(SimResidency::kClass));
static_assert(!MayFeedWorldHash(RenderResidency::kClass));
```

Named tag types `SimResidency` / `RenderResidency` (empty structs carrying `kClass`) give
the exact identifiers the spec is checked against, without imposing storage or wiring. The
runtime `enum class ResidencyClass { Sim, Render }` is available for switch/registry use.

> **OQ-1 is open.** Whether the partition ultimately becomes a compile-time type
> distinction (a `Tagged<T, Class>` wrapper so a render value *cannot* be passed to a
> hash-feeding function) or a runtime-asserted registry is left to the consuming spec.
> This header declares the vocabulary for **both**; it does **not** force the invasive
> wrapper onto every signature.

### 2a. Config residency parity (FR-A-004)

`ResidencyClass` maps **1:1** to `SystemConfig`'s `enum class Section { Sim, Render }`
(`src/luminumbra_common/core/SystemConfig.cpp:15`):

- `Section::Sim` ↔ `ResidencyClass::Sim` — eligible to feed a hash.
- `Section::Render` ↔ `ResidencyClass::Render` — `render.*` is **never** hashed
  (`SystemConfig.cpp:223`).
- An all-sim-default config hashes to the **empty baseline** (`SystemConfig.cpp:243`), so
  an unconfigured world is byte-identical and additive (zero re-pin).

This is the same rule on both sides of the partition: the config sub-hash and the residency
contract agree by construction.

---

## 3. The deterministic-input invariant (FR-A-002)

Any value that feeds `ServerWorldRunner::ComputeWorldHash` (`ServerWorldRunner.cpp:510`) or
`ComputeWorldSubHashes` (`:536`) must be derivable **purely** from

> `(seed, preset, deterministic-config, tick)` + position, and the deterministic
> availability set (§4).

…and **never** from wall-clock, job-completion order, thread scheduling, or GPU readback.
That tuple is exactly `AvailabilityKey` in the header:

```cpp
struct AvailabilityKey {
    std::uint64_t seed;             // world seed
    std::uint32_t preset_id;        // worldgen preset identity
    std::uint64_t config_sub_hash;  // deterministic config sub-hash; 0 == empty baseline
    std::int64_t  tick;             // sim tick index
    std::int32_t  chunk_x, chunk_y, chunk_z;  // residency position
};
```

`config_sub_hash` carries `SystemConfig::ComputeConfigSubHash` (`SystemConfig.cpp:217-244`):
render-excluded, sim-only-when-enabled, empty-baseline when unconfigured — so the key is
byte-stable for a default world.

---

## 4. The deterministic availability set (FR-B-001/002/003/004)

**Definition.** At each sim tick `T`, the set of chunks/resources a deterministic system
may read is the *settled residency* produced by the per-tick streaming barrier
(`world_system->wait_for_streaming_jobs()`, `ServerWorldRunner.cpp:516`/`:546`/`:591`) plus
the boot-settle of initial residency (`ServerWorldRunner.cpp:369-393`). The sim advances
from this set in a **deterministic order**, *not* from the order async jobs happen to
finish.

**Membership is a pure function of `AvailabilityKey`.** The header fixes the contract's
*shape* (not its implementation), so it stays behavior-neutral:

```cpp
//  IsResident(key)  ::  pure fn of AvailabilityKey
//                       — NEVER wall-clock, job-completion order, thread schedule, or
//                         GPU readback.
using IsResidentFn = bool (*)(const AvailabilityKey& key);
```

**FR-B-002 — the barrier is the only legal availability source.** No deterministic system
reads chunk/streamed state outside the settled set (i.e. it must not observe a chunk whose
generation/meshing has not quiesced for `T`). The barrier sites above are the sanctioned
entry points; new streamed sim systems consume residency through the same barrier.

**FR-B-003 — Spec 017-B consumption clause.** When 017 replaces the main-thread
`wait_for_streaming_jobs()` with async ownership / per-tick completion budgets, the sim must
still advance from this availability set — a stable, tick-keyed activation queue keyed on
`AvailabilityKey`. 017 may change *how* residency settles; it must not change *what the sim
sees per tick*. If 017 alters residency mechanics, these assertions are revalidated against
the new mechanism (the contract is *what the sim sees per tick*, which 017 must preserve).

**FR-B-004 — bed/sampler purity carry-forward.** Deterministic systems read terrain through
the **pure scalar sampler** (`GetTerrainHeightAt`, gated `current_lod == 0`), never through
shared mutable streaming buffers (`heightmap_data`). This generalizes the water lesson
(`docs/water-sim-lockstep-determinism.md:50-61`; memory: water-heightmap-read-race): the
shared-buffer read was a *data race*, not a value mismatch — the pure sampler is race-free.

---

## 5. Streamed-system residency pattern (water is the template) — FR-C

Water is the documented exemplar of a streamed sim system that is also a **halting peer
hash oracle** (its sub-hash is exchanged and halts the lockstep session on mismatch —
`docs/water-sim-lockstep-determinism.md:38-46`). Any new streamed sim system (foliage
growth, erosion, finite hydrology, ecology) inherits the pattern rather than re-deriving it:

1. **Boot-settle** the initial residency to a trajectory-independent steady state on the
   shared boot path both peers run (`ServerWorldRunner.cpp:369-393`).
2. Consume per-tick residency through the **streaming barrier** (§4), not job order.
3. Read terrain via the **pure sampler** (FR-B-004).
4. Prove it with **`--smoke`** (initial residency) **and `--smoke-moving`** (chunks
   streaming in/out during the run), since boot-settle alone does not cover the moving case.
5. A halting-oracle system folds its sub-hash into the exchanged set only **after** it
   passes 1–4 (FR-C-003).

---

## 6. What this contract does NOT do (neutrality guard)

- It adds **no** hash input, folds **nothing**, and is **included by no TU** yet — so
  `--smoke == 6f008a9f637c40b7` and run==replay are unaffected (NFR-001).
- It does **not** modify `ServerWorldRunner.cpp`, `SystemConfig.*`, `main_server.cpp`, or
  any `CMakeLists.txt`. Wiring (the FR-A-002 docstring at the hash-assembly site, the
  FR-A-003 partition-driven match set, the FR-E readback discipline, and the FR-F audit
  gate) lands in later 018 tracks and in Spec 017-B.
- GPU-readback paths (Spec 018 FR-E cites `RenderPipeline.cpp` / `FoliagePass.cpp`,
  *unverified here*) are classified `RenderResidency` by that later track; this contract
  only states the rule (`MayFeedWorldHash(Render) == false`).
