# Spec 010 — Finite Hydrology (rain fills / evaporation drains)

Status: Landed (default-OFF; activation is an owner decision)
Owner: Lead Architect (water/sim)
Determinism class: HASHED SIM when enabled (integer mm domain; lockstep host==peer + run==replay)
Authored retroactively (Wave G W1.4, 2026-07-05): the implementation shipped in `524a5557`
with no spec directory — this document records what was built and its contract. WATER-14.

---

## Problem

Spec 009's flowing water uses source/sink boundary conditions: river sources inject water
forever, sinks absorb it forever. That is the right model for *rivers* but makes every water
body effectively infinite — a drained lake refills from nothing, a dammed pool never dries.
The finite-hydrology layer closes the loop: **rain fills, evaporation drains, no perpetual
source** — the total land-water budget becomes a real, finite, weather-driven quantity.

## What shipped (the contract)

- **`WaterSystem::SetHydrology(finite, rain_mm_per_tick, evap_mm_per_tick)`** — the single
  activation seam. `finite=false` (the DEFAULT) is byte-identical to spec-009 behavior:
  the flag and parameters exist but the solver path is untouched.
- When `finite=true`:
  - **Rain**: `rain_mm_per_tick` is added to every *land-water-probe-gated* cell per tick,
    in the integer mm domain (no float accumulators — the value is a per-tick integer add,
    deterministic by construction).
  - **Evaporation**: `evap_mm_per_tick` is subtracted from wet cells, clamped at zero.
  - **Perpetual sources demoted**: river source injection respects the finite budget rules
    so a finite world's water total is driven by rain − evap, not by infinite boundary
    conditions.
- **Land-water-probe gating**: rain lands only on cells the land-water probe classifies as
  land-water candidates (not ocean/sea-level cells), so the finite budget applies to inland
  hydrology, not the boundary ocean.

## Determinism

- All mutations are integer mm; the per-tick order is the solver's existing id-sorted cell
  order. Two runs with the same seed + flags produce identical `debug_water_state_hash`
  sequences (covered by the WaterDeterminism suite pattern).
- Default-OFF means the canonical smoke baselines are UNTOUCHED — activation is a deliberate
  hash bump (an owner-menu item, per the spec-021 campaign's activation-bump discipline).

## What is NOT in this spec (chartered follow-ups)

- **Weather-driven rain** (spec-021 rank 86, ATMO-11 ≡ WATER-07, Wave G S1.1): replace the
  globally-uniform `rain_mm_per_tick` with per-cell deterministic quantization of
  `WeatherSystem::PrecipitationAt(cell)` behind `sim.hydrology_weather` (default-OFF).
  Integer-quantize AT THE BOUNDARY (`int(precip * scale + 0.5)`), then the existing mm
  solver — the same one-tick-phase pattern scent uses for wind.
- **WeatherEvent modulation** (ATMO-12): storm/drought epochs scaling the rain input.
- Groundwater / soil absorption: not designed; would be a new spec.

## Proving signals

- The existing WaterDeterminism suite (run==replay + mass invariant) with hydrology OFF —
  the shipped default — plus the spec-021 canonical smokes byte-identical.
- On activation (future bump): a storm epoch raises total land-water volume; a drought
  drains it; run==replay holds (`WaterDeterminism.WeatherDrivenRainIsDeterministic`,
  authored with S1.1).
