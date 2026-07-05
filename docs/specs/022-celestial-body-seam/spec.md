# Spec 022 — Celestial-Body Seam (Tier 1)

**Status:** Landed (Tier 1, 2026-07-05, Wave F F9). Tier 2 charted, not built.
**Owner ask (2026-07-04):** "shouldn't we generalize concepts like sun/moon/etc even
further within the engine?"

## Problem

Sun and moon are hard-coded concepts inside `update_time_of_day` + RenderPipeline
members (`m_sun`, `m_moonLightDir`, `m_moonUpFactor`, `m_moonIllumination`). RENDER-14
(Wave E) already extracted the pure math (`TimeOfDayModel.h`: ComputeSunGeometry /
ComputeMoonGeometry / ComputeMoonIllumination) — the groundwork. What's missing is the
SEAM: one evaluation over a list-shaped celestial primitive, so a second moon, a bright
planet, or an aurora-driving source becomes DATA instead of new pipeline members.

## Tier 1 (THIS spec — byte-identical by construction)

- **FR-022-1:** `rendering/CelestialBodyModel.h` declares `CelestialBodyParams`
  (radiance model tag: TransmittanceCoupledSun | AuthoredNightFill; shadow role:
  PrimaryCascade | None; lunar-phase flag) + `CelestialBodyState` (travel/light
  directions, up factor, elevation, day factors, phase illumination) +
  `EvaluateCelestialBodies(timeOfDay, sunDeclination, seasonTick, moonOverride,
  ticksPerLunarCycle) -> CelestialFrame{sun, moon}`.
- **FR-022-2 (the load-bearing constraint):** the evaluation calls the EXISTING
  TimeOfDayModel primitives VERBATIM — including the byte-fragile trig asymmetry (the
  sun uses unqualified `::sin`; the moon uses `std::sin` float overloads). NO
  normalization; the seam is plumbing, not math.
- **FR-022-3:** `update_time_of_day` consumes the frame (one call replaces the three
  primitive call sites); the lighting/shadow passes are untouched — still one primary
  cascade key + the moon fill. Pixels are BIT-IDENTICAL.
- **Gate:** `CelestialBodyModel.SunMoonSeamBitExactAgainstPrimitives` — a tod x
  declination x tick sweep asserting bit-equality of every state field against direct
  primitive calls, plus both determinism smokes byte-identical and
  `-Mode RenderParityFrame` == 0.0.

## Tier 2 (charted, NOT built — rank in the render band behind RENDER-18)

True N-body: a `std::vector<CelestialBodyParams>` the pipeline iterates; >1
shadow-casting celestial light means a genuinely new multi-key lighting + shadow-pass
feature. Needs: the whole-frame A/B (exists since Wave F F1), a WorldVisualSweep
re-bless, and it interacts with resource ownership + the RHI track. ATMO-10 (sim-tick
time authority) is the prerequisite for any celestial quantity becoming SIM-authoritative
— today TOD is scenario/wall-clock and celestial render state must never feed
world_hash (018 FR-E-003).
