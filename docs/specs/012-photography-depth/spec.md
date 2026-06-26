# Spec 012: Photography — Mechanical Depth (Light, Lens Craft & Behaviour)

> Status: IN PROGRESS (created 2026-06-26). The **co-equal spine** the Living-World
> devil's-advocate critique demanded: spec 011 makes the *world* worth photographing;
> this spec makes *photographing it* a skill with depth. Builds on the existing photo
> layer (`PhotoSession`/`PhotoMode`/`PhotoCamera`/`PhotoCodex`/`Objectives`) and depends
> on spec 011 Phase G (rest-pose visual) for the behaviour-objective payoff.
>
> **The load-bearing invariant:** the photo layer is **pure / client-only** and has
> **never** touched `world_hash` — `ComposeWorldHash` (ServerWorldRunner.cpp:97-112) has
> no photo/codex term. So **every requirement in this spec is determinism-NEUTRAL by
> construction**: no new sim term, no seed offset, no re-pin. The whole spec moves
> within client photo state.
>
> **Landed 2026-06-26:**
> - **Phase 2 — verdict rebalance** (FR-B): weights moved from comp0.40/exp0.30/focus0.30
>   to **comp0.35 / exp0.25 / focus0.40** (PhotoSession.h:85-87) so deliberate DoF is the
>   highest-value decision, and the two optical axes now blend **0.6 optics / 0.4 rubric**
>   (kExpOptics/kFiOptics = 0.6, PhotoSession.h:95-98). The old weighting made the
>   aperture/focus and manual-exposure mechanics nearly weightless. Guarded by the
>   `WellComposedDeepBeatsBadlyComposedShallow` test. Pure layer → byte-identical.
> - **Phase 4 (logic) — observation metadata** (FR-D1/D2): `ObservationMetadata`
>   (subject_action / time_of_day / scene_luminance, PhotoCodex.h:48) is now stored on
>   each capture (`PhotoSession.observation`, PhotoSession.h:61) and serialized into the
>   `PhotoSidecar` JSON (`observation` field, PhotoMode.h:195). Pure layer → byte-identical.
> - **Phase 5 — behaviour objectives** (FR-E1/E2/E3): `ObjectiveKind::BehavioralMatch`
>   (Objectives.h:34) matches a captured subject's brain action; `PhotoCodex` carries a
>   per-species `behaviour_mask` (PhotoCodex.h:99); a starter objective "Photograph a
>   sleeping creature" (target_action = 5 = Sleep, Objectives.h:189) ships. Pure layer →
>   byte-identical.
>
> **Pending (all client-side wiring, all determinism-neutral):**
> - **Phase 1 — light-driven exposure** (FR-A): the scene luminance fed to scoring is the
>   hardcoded `pv.light = 0.6f` (main_client.cpp:568). Needs a `RenderPipeline::time_of_day()`
>   getter (the member `m_timeOfDay` ~line 904 and `set_time_of_day` ~line 607 exist; there
>   is **no getter**) and a luminance derived from sun elevation so golden hour rewards and
>   midnight punishes.
> - **Phase 3 — manual exposure mechanic** (FR-C): `aperture`/`focus` are already wired at
>   input (`consume_aperture_nudge`/`consume_focus_nudge`, PlayerController.h:87-96, applied
>   main_client.cpp:6642-6647); **shutter + ISO are unwired**. Needs the same input plumbing
>   plus a render-side EV/histogram HUD (read-only). `ExposureValue` already consumes
>   shutter+iso via the libm-free `Log2Approx` — so this is **input plumbing + a HUD**, not
>   new scoring math.
> - **Phase 4 (client population)** (FR-D3): the pure layer accepts metadata; the client must
>   read the subject's `CreatureAction` and the time-of-day **at capture** and populate it.
> - **Phase G dependency** (FR-E4): "sleeping creature" only photographs meaningfully once
>   spec 011 Phase G gives sleep a visible rest pose.
>
> **Next:** Phase 1 (luminance) → Phase 3 (manual exposure + histogram) → Phase 4 client
> population → focus-lock stretch.

## Context

The photography loop — Project Capture's whole reason to exist — has the *plumbing* of a
camera but, before this spec, not the *depth* of one. The devil's-advocate critique on the
Living-World direction made the point sharply: a richer world is only half the game if
**capturing** it stays shallow. This spec is the other half of that spine.

Where the photo layer stands today:

- **Scene light is a constant.** Every photographed subject is scored against
  `pv.light = 0.6f` (`main_client.cpp:568`) — a hardcoded "scene-luminance proxy". The
  renderer *knows* the time of day (`RenderPipeline::set_time_of_day`, member `m_timeOfDay`)
  but exposes **no getter**, so the photo path can't read it. The result: a midnight frame
  and a golden-hour frame score their exposure **identically**. The single biggest lever in
  real photography — light — is inert.
- **The verdict taught the wrong lesson.** The old weights (comp0.40/exp0.30/focus0.30)
  ranked composition above optics, which made the aperture/focus decision and any future
  manual-exposure decision nearly weightless. (Rebalanced — see FR-B, landed.)
- **Half the lens is unwired.** `LensSettings` (PhotoCamera.h:40-46) carries the full
  manual quintet — `focal_length_mm`, `aperture_f`, `focus_distance_m`, `iso`, `shutter_s`.
  `aperture` and `focus` are wired to player input and applied each frame
  (PlayerController.h:87-96 → main_client.cpp:6642-6647). **`shutter_s` and `iso` are not**
  — yet `ExposureValue` already *consumes* both (via the libm-free `Log2Approx`). The
  manual-exposure mechanic is therefore one input-plumbing pass plus a read-only HUD away,
  not a scoring rewrite.
- **Captures forgot their context.** A photo recorded *what* (subject) and *how well*
  (verdict) but not the *moment*: what the creature was doing, when, in what light. (The
  pure `ObservationMetadata` struct + sidecar serialization landed — see FR-D — but the
  client doesn't yet *fill it in* at capture.)
- **Objectives couldn't reward behaviour.** Until `BehavioralMatch` (landed, FR-E),
  objectives keyed only on species discovery / star rating — never on catching a creature
  mid-action. "Photograph a sleeping creature" is the first behaviour objective, and it's the
  hand-off point where this spec meets spec 011: it only pays off once sleep *looks* like
  sleep (spec 011 Phase G).

This spec turns the camera from a screenshot button into an instrument: **light drives
exposure**, **lens craft is the highest-value skill**, **manual exposure is a real
(read-only-scored) mechanic with a histogram**, **captures remember the moment**, and
**objectives reward behaviour, not just presence** — all without touching a single sim term.

## Goals

- **Light is the lever.** Replace the constant `0.6f` scene-luminance with a value derived
  from the renderer's time-of-day / sun elevation: peak (~0.7) at golden hour, trough
  (~0.1) at midnight — so *when* you shoot changes the exposure verdict, the way it does
  with a real camera.
- **Lens craft leads.** The verdict rewards deliberate optics: focus/isolation is the
  top-weighted axis and the two optical axes lean on the lens facts (0.6 optics), so a
  thoughtful DoF/exposure choice is the highest-value decision a player makes.
- **Manual exposure is a real mechanic.** The full manual quintet is player-controllable;
  a live histogram / EV readout teaches the exposure triangle — **as feedback only**, never
  feeding the score or the world hash.
- **Captures remember the moment.** Each photo records the subject's action, the time of
  day, and the scene luminance, serialized into the sidecar — the substrate for behaviour
  objectives, codex behaviour unlocks, and "what was happening when I took this".
- **Objectives reward behaviour.** A `BehavioralMatch` objective completes when you
  photograph a subject performing a target action ("a sleeping creature"), tying the
  photography loop directly to the Living-World daily-life loop (spec 011).
- **Determinism untouched, by construction.** Because the entire photo layer is pure /
  client-only and absent from `ComposeWorldHash`, every requirement here is
  determinism-neutral: no sim term, no seed offset, no re-pin, `world_hash` byte-identical.

## Non-Goals

- **No `world_hash` participation, ever.** Nothing in this spec adds a sim term, a seed
  offset, or a `Compute<X>SubHash`. Photo state stays pure / client-only. (This is the
  invariant, not an aspiration.)
- **No physically-based film/sensor simulation.** Exposure stays the existing
  `Log2Approx`-based EV model; no spectral rendering, no real sensor noise/grain model,
  no per-pixel tonemap rewrite. The histogram reads what the renderer already produced.
- **No scoring rewrite.** The verdict math (three axes, optics blends) is set by FR-B and
  is **landed**; later phases plug *better inputs* (real luminance) into it, they do not
  re-derive it. The histogram never feeds the score.
- **No new skeletal-rig or animation work** — the "sleeping creature" pose is spec 011
  Phase G's deliverable; this spec only *reads* the resulting `CreatureAction`.
- **No new RmlUi screen.** The histogram/EV HUD is an in-photo-mode overlay (ImGui/existing
  HUD draw), not a new UI document.
- **No perf-frontier work.** The only new GPU cost (the histogram readback) is budgeted in
  the photo-mode HUD path, **not** the perf-gated `forest_dense` render path.

## Functional Requirements

### A. Light-driven exposure (CLIENT — pure, determinism-neutral)

- **FR-A1 (time-of-day getter).** Add a `RenderPipeline::time_of_day()` (or sun-elevation)
  **getter**. The setter `set_time_of_day` (~RenderPipeline.h:607) and the member
  `m_timeOfDay` (~RenderPipeline.h:904) exist; there is no read accessor, so the photo path
  cannot see the light. Read-only, const, no behaviour change. **(PENDING.)**
- **FR-A2 (luminance from light).** Derive the scene luminance from sun elevation / time of
  day — a smooth curve peaking ~**0.7** at golden hour and bottoming ~**0.1** at midnight —
  and use it to replace the hardcoded `pv.light = 0.6f` in `GatherPhotoSubjects`
  (main_client.cpp:568). Pure float math on the client; no libm in any pure header (the
  curve may live client-side, where libm is allowed, or use `Log2Approx`-style approximation
  if it touches a pure header). **(PENDING — client wiring.)**
- **FR-A3 (luminance flows both ways).** The derived luminance feeds **both** the
  `PhotoScoring` lighting axis **and** the `PhotoCamera` `ExposureQuality` so that the
  *same* light that the player sees is the light the verdict judges and the histogram
  reflects — one source of truth for "how bright is this scene". **(PENDING.)**

### B. Scoring rebalance toward lens craft (CLIENT — pure) — **LANDED**

- **FR-B1 (verdict weights).** Verdict weights are **comp 0.35 / exp 0.25 / focus 0.40**
  (PhotoSession.h:85-87, was 0.40/0.30/0.30) so deliberate focus/DoF is the top-weighted
  decision. **(LANDED.)**
- **FR-B2 (optics-led axis blends).** The two optical axes fold the lens facts in at
  **0.6 optics / 0.4 rubric** (`kExpOptics` / `kFiOptics` = 0.6, PhotoSession.h:95-98):
  `exposure = 0.4·lighting + 0.6·ExposureQuality`,
  `focus_iso = 0.4·focus + 0.6·SubjectIsolation`. The rubric still anchors each axis so
  neither is pure-optics. **(LANDED.)**
- **FR-B3 (guard test).** A test asserts a **well-composed deep f/8** capture beats a
  **badly-composed shallow f/1.4** one — so creamy bokeh cannot win on lens alone and
  framing still matters (`WellComposedDeepBeatsBadlyComposedShallow`). **(LANDED.)**

### C. Manual-exposure mechanic (CLIENT input + RENDER HUD — pure)

- **FR-C1 (wire shutter + ISO).** Add player nudges for `shutter_s` and `iso` mirroring the
  existing `consume_aperture_nudge` / `consume_focus_nudge` (PlayerController.h:87-96) and
  apply them where aperture/focus are applied (main_client.cpp:6642-6647). `ExposureValue`
  already consumes both via `Log2Approx`, so this is **input plumbing only** — no new
  scoring math. **(PENDING.)**
- **FR-C2 (histogram / EV HUD).** A render-side, **read-only** histogram + EV readout in
  photo mode: the current exposure value, the luminance distribution, clipping indicators.
  It teaches the exposure triangle (shutter/ISO/aperture) as *feedback*. It **never feeds
  the score and never touches `world_hash`** — it is purely a display of state the renderer
  already produced. Budgeted in the photo-mode HUD path, not `forest_dense`. **(PENDING.)**
- **FR-C3 (focus-lock — STRETCH).** A half-press focus lock + focus-peaking overlay
  (highlight the in-focus band), reusing the existing focus-distance plumbing. Quality-of-
  life; ship only after C1/C2. **(STRETCH / PENDING.)**

### D. Observation metadata (CLIENT — pure)

- **FR-D1 (struct stored with capture).** `ObservationMetadata`
  (subject_action / time_of_day / scene_luminance, PhotoCodex.h:48) is stored on the capture
  (`PhotoSession.observation`, PhotoSession.h:61). **(LANDED.)**
- **FR-D2 (serialized in sidecar).** The metadata is written into the `PhotoSidecar` JSON
  as an `observation { subject_action, time_of_day, scene_luminance }` block
  (PhotoMode.h:195, via `SerializePhotoSidecar`). **(LANDED.)**
- **FR-D3 (client populates at capture).** At capture time the client reads the subject
  creature's `CreatureAction` and the renderer's time-of-day (FR-A1) and the derived scene
  luminance (FR-A2), and fills the `ObservationMetadata` — so the stored/serialized values
  are real, not defaults. **(PENDING — depends on FR-A1/A2.)**

### E. Behaviour objectives (CLIENT — pure)

- **FR-E1 (BehavioralMatch objective kind).** `ObjectiveKind::BehavioralMatch`
  (Objectives.h:34) completes when a captured subject's brain action equals the objective's
  `target_action` (Objectives.h:104), `species_id = 0` meaning "any species". **(LANDED.)**
- **FR-E2 (codex behaviour mask).** When a behaviour-tagged capture is recorded, its
  behaviour bit is OR'd into the species' `behaviour_mask` in the `PhotoCodex`
  (PhotoCodex.h:99) — so the codex remembers *which behaviours* you've documented per
  species, not just that you've seen it. **(LANDED.)**
- **FR-E3 (starter objective).** A shipped objective "Photograph a sleeping creature"
  (`target_action = 5 = Sleep`, Objectives.h:189) is the first behaviour objective.
  **(LANDED.)**
- **FR-E4 (Phase-G dependency).** The starter objective only *reads* meaningfully once spec
  011 **Phase G** gives the Sleep action a visible rest pose — otherwise the player can't
  *tell* a creature is sleeping to aim at it. This is a cross-spec ordering note, not new
  code here. **(BLOCKED on spec 011 Phase G.)**

## Non-Functional Requirements

- **NFR-1 (determinism-neutral by construction).** The photo layer is pure / client-only
  and absent from `ComposeWorldHash` (ServerWorldRunner.cpp:97-112). No requirement here
  adds a sim term, a seed offset, or a sub-hash. `world_hash` stays **byte-identical**; no
  re-pin is needed for any phase. This is the spec's defining property.
- **NFR-2 (libm-free discipline in pure headers).** No `std::pow` / `std::log` /
  `std::exp` in the pure photo headers (`PhotoCamera.h`, `PhotoScoring.h`, `PhotoSession.h`,
  `PhotoMode.h`, `PhotoCodex.h`, `Objectives.h`) — exposure math uses the existing
  `Log2Approx`. Float curves that need real libm (e.g. the luminance-from-elevation curve)
  live **client-side** (`main_client.cpp` / render code), not in a pure header.
- **NFR-3 (histogram budget).** The FR-C2 histogram readback is budgeted in the
  **photo-mode HUD** path and is gated to photo mode only; it must **not** add cost to the
  perf-gated `forest_dense` render benchmark. Prefer a cheap source (a downsampled lit mip
  or a CPU bucket of the already-resolved frame) over a full-frame readback.
- **NFR-4 (read-only scoring boundary).** The histogram/EV HUD and the time-of-day getter
  are strictly read-only with respect to scoring: the verdict is a pure function of
  `PhotoScoring` + `PhotoCamera`, and the HUD is downstream of it. The HUD can never change
  the score.
- **NFR-5 (audio).** Any new photo-mode audio (e.g. a shutter/aperture-detent click) uses
  **mp3**, not `.ogg` (miniaudio has no Vorbis decoder), via the `tools/audio` pipeline into
  `sfx_main`, per `docs/AUDIO-everything-maps-to-sound.md`. (No new audio is strictly
  required by this spec; if added, it follows the standing rule.)

## Acceptance Criteria

- [ ] **AC-1 (light changes exposure).** A golden-hour capture and a midnight capture of the
      same subject yield **different** exposure verdicts (golden hour ~0.7 luminance scores
      higher / differently than ~0.1 midnight) — the hardcoded `0.6f` is gone.
- [ ] **AC-2 (manual exposure is felt).** Changing shutter and/or ISO via player input
      changes the **EV readout / histogram** and the `ExposureQuality` input to the score —
      the player can over- and under-expose deliberately.
- [x] **AC-3 (lens craft leads).** A poorly-composed **f/1.4** capture loses to a
      well-composed **f/8** one (`WellComposedDeepBeatsBadlyComposedShallow`). **(LANDED.)**
- [x] **AC-4 (sidecar carries context).** The sidecar JSON contains
      `observation { subject_action, time_of_day, scene_luminance }`. **(LANDED — struct +
      serialization; values become real once FR-D3 populates them.)**
- [ ] **AC-5 (behaviour objective completes).** `BehavioralMatch(Sleep)` completes when the
      player photographs a sleeping creature (requires spec 011 Phase G for a visible rest
      pose to aim at). *(Objective logic LANDED; end-to-end completion gated on Phase G.)*
- [ ] **AC-6 (tests green).** `common_tests` green (photo-scoring / sidecar / objective /
      codex fixtures unchanged or extended).
- [ ] **AC-7 (determinism untouched).** `--smoke` run==replay and `world_hash`
      **byte-identical** — proving the photo-depth work never reached the sim (the invariant).
- [ ] **AC-8 (no perf regression).** The histogram HUD adds no cost to the `forest_dense`
      render benchmark (it is photo-mode-only); `--render-benchmark` within budget.

## Suggested phasing (each independently shippable; none re-pins the hash)

1. **Phase 1 — Light-driven exposure** (FR-A): add `time_of_day()` getter, derive luminance
   from sun elevation, replace `0.6f`, flow it into scoring + `ExposureQuality`. *(PENDING —
   the highest-leverage missing piece.)*
2. **Phase 2 — Scoring rebalance** (FR-B): weights + optics-led blends + guard test.
   **(LANDED.)**
3. **Phase 3 — Manual-exposure mechanic** (FR-C): wire shutter+ISO input, add the read-only
   histogram/EV HUD. *(PENDING.)*
4. **Phase 4 — Metadata client population** (FR-D): struct + sidecar **(LANDED)**; client
   reads subject action + time-of-day at capture and fills it *(PENDING, depends on Phase 1)*.
5. **Phase 5 — Behaviour objectives** (FR-E): `BehavioralMatch`, behaviour mask, starter
   "sleeping creature" objective. **(LANDED — payoff gated on spec 011 Phase G.)**
6. **Stretch — Focus lock + focus-peaking** (FR-C3): half-press lock + in-focus overlay,
   after the core mechanic ships.

## Open Questions

- **OQ-1 (histogram source).** Sample a **downsampled lit mip** the renderer already
  produces (cheap, GPU-side, but a mip read) vs a **CPU bucket** of the resolved frame
  (simple, but a readback)? Pick the one that stays out of the `forest_dense` budget (NFR-3).
- **OQ-2 (objective gating).** Should behaviour objectives ("photograph a sleeping
  creature") be **tier-gated behind discovery** (you must have *discovered* the species
  first) or available immediately as a teaching prompt? Gating ties depth to progression but
  may hide the mechanic early.
- **OQ-3 (exposure input granularity).** Manual shutter/ISO/aperture in discrete
  **real-camera stops** (1/3-stop detents — readable, photographic, clickable) vs
  **continuous** nudges (smooth, but less "instrument-like")? Per-stop matches the histogram
  teaching goal; continuous matches the existing aperture/focus nudge feel.

## References (grounding — current code)

- **Purity invariant:** `src/luminumbra_server/ServerWorldRunner.cpp:97-112`
  (`ComposeWorldHash` — **no** photo/codex term); the pure photo headers under
  `src/luminumbra_common/game/`: `PhotoSession.h`, `PhotoMode.h`, `PhotoCamera.h`,
  `PhotoCodex.h`, `Objectives.h`.
- **Scene luminance (FR-A):** hardcoded proxy `pv.light = 0.6f` in `GatherPhotoSubjects`,
  `src/luminumbra_client/main_client.cpp:568`; renderer time-of-day setter + member but
  **no getter** in `src/luminumbra_client/rendering/RenderPipeline.h` (`set_time_of_day`
  ~607, `m_timeOfDay` ~904).
- **Verdict weights / optics blends (FR-B — landed):**
  `src/luminumbra_common/game/PhotoSession.h:78-89` (`kVerdictW*` = 0.35/0.25/0.40) and
  `:95-98` (`kExpRubric`/`kExpOptics`/`kFiRubric`/`kFiOptics` = 0.4/0.6/0.4/0.6).
- **Lens / exposure (FR-C):** `src/luminumbra_common/game/PhotoCamera.h:40-46`
  (`LensSettings`: `focal_length_mm`, `aperture_f`, `focus_distance_m`, `iso`, `shutter_s`),
  `ExposureValue` + `Log2Approx` (same header); aperture/focus input
  `src/luminumbra_common/.../PlayerController.h:87-96` (`consume_aperture_nudge` /
  `consume_focus_nudge`), applied `src/luminumbra_client/main_client.cpp:6642-6647`.
- **Observation metadata (FR-D — landed):**
  `src/luminumbra_common/game/PhotoCodex.h:48` (`ObservationMetadata`),
  `PhotoSession.h:61` (`observation` on the capture),
  `PhotoMode.h:190-235` (`PhotoSidecar` `observation` field + `SerializePhotoSidecar`).
- **Behaviour objectives (FR-E — landed):**
  `src/luminumbra_common/game/Objectives.h:34` (`ObjectiveKind::BehavioralMatch`), `:44`/`:47`
  (`species_id` / `target_action`), `:104` (match logic), `:189` (starter "sleeping
  creature", `target_action = 5 = Sleep`); `PhotoCodex.h:99` (`behaviour_mask`).
- **Cross-spec dependency:** spec 011 (`docs/specs/011-living-creatures-daily-life/spec.md`)
  **Phase G** rest-pose visual — required for FR-E4 / AC-5.
- **Audio (NFR-5):** `docs/AUDIO-everything-maps-to-sound.md`, `tools/audio/`,
  `data/audio/sfx_main.bank.json`.
