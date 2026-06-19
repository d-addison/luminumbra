# Spec — Foliage polish (pillar A): deterministic procedural plant geometry

Status: DRAFT. Owner direction: HANDOFF §3.A (revised order §0.5). Tier: T2.
Branch: `feat/polyglot-audit-roadmap` (LOCAL-ONLY). Engine-generic; plant params are game data.

## Context

The Living-World Foliage pillar grows plants in the sim (genome → phenotype → ticked growth,
`systems/PlantGrowthSystem.h`), but the VISUAL is **baked static tree models** scaled by maturity —
there is no TRUE structural genetic variation. **Owner direction (2026-06-18): drop the baked
static models; grow plants PROGRAMMATICALLY + GENETICALLY + ATMOSPHERICALLY** (e.g. branches grow
toward the sun). Pillar A adds a **procedural plant-geometry generator**: a PURE function of
`(genome, growth stage, atmosphere)` → branch skeleton, so two genomes grow structurally different
plants, a plant visibly elaborates as it matures, and a plant **leans toward its light**.

**Determinism boundary (docs/STANDARDS.md §4):** procedural geometry is **VISUAL-ONLY** — it
never feeds the sim and never touches `world_hash`. But it MUST be a deterministic pure function
(same input → identical bytes) so a baked cache is reproducible and a future GPU/CPU split agrees.
To be cross-platform bit-stable it uses **only** `Luminumbra::DeterministicMath` (libm-free
Sin/Cos/Sqrt) + IEEE basic ops — no `std::sin`/`glm::rotate` (libm-backed, platform-divergent).

This spec's FIRST SLICE is the generator + its determinism gate (headlessly unit-testable). The
mesh-cache bake, impostor atlas, and scatter integration (gated by `render.plant_procgen.enabled`,
OFF by default) are follow-on slices.

## Requirements

- **R-A1** New header `systems/PlantProcgen.h` (header-only, dependency-light like
  `PlantGrowthSystem.h`), namespace `luminumbra::foliage`. API:
  `std::vector<glm::vec3> GeneratePlantMesh(const Comp::PlantGenomeComponent& genome, std::uint8_t stage)`
  returning a **line-list branch skeleton** (pairs of endpoints). Visual-only.
- **R-A2 (purity/determinism)** `GeneratePlantMesh` is a PURE function: identical `(genome, stage)`
  → identical vertex bytes, every call, on any platform (uses `DeterministicMath` only).
- **R-A3 (genetic structure)** Different genomes produce different geometry (branch count, angle,
  length ratio, total length derive from the genome / its `ExpressGenome` phenotype).
- **R-A4 (maturation)** Geometry complexity is **non-decreasing in stage** (a later stage has ≥
  the vertex count of an earlier stage) — the plant elaborates as it grows. Bounded (max depth,
  max branch factor) so vertex count stays sane.
- **R-A6 (atmospheric / phototropism)** The generator takes an atmosphere input `PlantEnvDir`
  (sun direction + phototropism strength; the renderer supplies it from the live day/night sun).
  Branches bend toward the sun, modulated genetically (leafier genomes seek light harder). Default
  env (sun up, no phototropism) reproduces the pure genetic form. Still a deterministic pure
  function of `(genome, stage, env)`.
- **R-A5 (flag gate, follow-on)** The scatter calls the generator only when
  `SystemConfig::enabled(render.plant_procgen)` (render-only flag, OFF by default); with the flag
  OFF the scatter is byte-identical to today. (Integration slice; the generator itself is always pure.)

## Acceptance Criteria (RED-first)

- **AC-A-001 (determinism)** `GeneratePlantMesh(g, s)` called twice returns vertex vectors that are
  equal element-by-element (bit-for-bit). Proving signal: `test/common/PlantProcgen_test.cpp`.
- **AC-A-002 (non-empty + bounded)** For a representative genome at the mature stage the result is
  non-empty and under a fixed vertex cap (e.g. ≤ 4096 verts). Proving signal: same.
- **AC-A-003 (maturation monotonic)** vertex_count(stage s+1) ≥ vertex_count(stage s) for all
  stages of a fixed genome. Proving signal: same.
- **AC-A-004 (genetic variation)** two distinct genomes at the same stage produce non-identical
  vertex vectors. Proving signal: same.
- **AC-A-005 (cross-platform stability)** a fixed `(genome, stage)` hashes to a pinned FNV-1a value
  (locks the libm-free determinism; a change is deliberate). Proving signal: same.
- **AC-A-006 (atmospheric response)** a leaning sun + nonzero phototropism produces geometry that
  differs from the default-env form, shifts the canopy toward the sun, and is itself deterministic.
  Proving signal: same.

## Addendum B — rich plant structure (landed)

Beyond the skeleton line list, `GeneratePlant(genome, stage, env) -> PlantStructure` produces what
a renderer tessellates into a real plant: **tapered branches with pipe-model radii** + **sun-facing
leaves** + a **sun-leaning trunk**. Still a pure, libm-free, deterministic function.
- **R-A7 (pipe-model thickness)** Branch radius follows the da Vinci / Borchert-Honda pipe model
  (`r_parent = sqrt(Σ r_child²)`), so the trunk is the thickest segment and twigs are thin.
- **R-A8 (leaves)** Terminal twigs carry leaves; leaf count grows with maturity and with a leafier
  genome; leaves face the sun under phototropism.
- **R-A9 (whole-plant phototropism)** The trunk itself leans toward the sun (half the per-branch
  strength), so the whole plant — not just the canopy — seeks light.
- ACs (all green): **A-007** trunk is the thickest branch; **A-008** leaf count monotonic in stage +
  leafier genome grows more; **A-009** trunk leans toward a non-vertical sun (deterministically);
  **A-010** pinned structure golden (`776720397350691645`).

## Addendum C — richer plant detail (track 3, planned)

Pure-generator extensions (headlessly testable; additive to `PlantProcgen.h`, do NOT break the
existing `GeneratePlant`/`TessellatePlant`/`ProcMesh` API while the render integration is in flight):
- **R-A10 (genetic + seasonal color)** Per-leaf and per-bark albedo derived from the genome
  (a color/chroma gene) and a seasonal phase input (spring green → autumn ochre/red → winter bare).
  Add an albedo to `ProcVertex` (additive field with a sane default) so the tessellation carries it.
  ACs: deterministic; autumn phase shifts leaf albedo toward red/ochre vs. summer; bare-winter drops
  leaf count.
- **R-A11 (2nd species)** A small data-driven species table (genome ranges + structural biases, e.g.
  conifer = low branch-tilt + tall, broadleaf = wide) so the world grows distinct species, not one
  shape rescaled. AC: two species at the same stage produce structurally distinct meshes; deterministic.
- **R-A12 (branch detail)** Optional taper-within-segment + slight deterministic gnarl so trunks read
  as organic, not perfect cones. AC: deterministic; vertex budget still bounded.

## Verify With
`cmake --build --preset debug --target common_tests` (msys64 PATH first) then
`ctest --test-dir build/debug -R "PlantProcgen" --output-on-failure` (10 tests).
