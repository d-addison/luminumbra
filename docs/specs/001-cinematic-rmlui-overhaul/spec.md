# Spec: Cinematic RmlUi Overhaul — Match the Reference Designs

## Context

`references/ui/` holds 8 polished concept frames for Luminumbra's interface (main menu,
world select, create world, settings, HUD, photo mode, pause, gallery). They share one
cohesive language: a **live golden-hour landscape behind every screen**, wide letter-spaced
lowercase serif titles, **translucent frosted-glass panels** with hairline borders and soft
shadows, thin sliders, an EXIF-style photo-mode readout, and real thumbnail grids.

The current RmlUi 6.1 implementation (`data/ui/*.rml` + 3 `.rcss`) is already in the right
design language — warm-dusk palette, Lora serif, minimal diegetic HUD/photo/pause — but sits
a notch below the references on fidelity. The linchpin cause: the hand-rolled `RmlRenderer`
never implements RmlUi's layered render API, so `backdrop-filter`, `filter`, and `box-shadow`
silently no-op today. RmlUi 6.1 ships a complete reference backend (`RmlUi_Renderer_GL3.cpp`)
that implements real gaussian blur, drop-shadow, and FBO layer compositing — adopting it turns
frosted glass and the cinematic backdrop into mostly-RCSS work.

The overhaul is driven by a per-screen `implement → screenshot → adversarial critique vs.
reference → iterate` loop (Claude Ultracode), run autonomously until critics agree each screen
matches the references' composition, typography, palette, and polish.

## Goals

- Each of the 8 screens visually matches its reference in composition, type, palette,
  frosted-glass treatment, and polish — judged by an adversarial visual critic loop.
- True frosted glass, soft shadows, and a live blurred world backdrop render correctly (via
  the GL3 backend), not silently no-op.
- A repeatable `--ui-screenshot <screen>` capture path feeds the automated visual gate.
- Real thumbnails in world-select and gallery (no `display:none` / gradient placeholders).
- Hot-reload wired so RCSS/RML iteration is sub-second.

## Non-Goals

- No migration to the unused `UIStateManager`/`UIProperty` data-binding layer — keep the
  existing imperative + `SettingsBridge` pattern (extend only for gallery/world-list population).
- No new fonts beyond Lora; no Vulkan; no gameplay/sim changes.
- Photo-mode capture pipeline logic untouched except adding a PNG writer + thumbnail emit.

## Functional Requirements

### Renderer (F1)
- FR-001: Adopt RmlUi 6.1 `RenderInterface_GL3` in place of the custom `RmlRenderer`, wired
  with `SetViewport` + `BeginFrame()`/`EndFrame()` around `context->Render()`.
- FR-002: Save/restore GL state (FBO binding, scissor, blend, viewport) around the UI pass so
  the world and ImGui passes are unaffected.
- FR-003: Image `src`, font, and `data/ui/...` paths continue to resolve through the file
  interface after the backend swap.

### Visual gate (F2)
- FR-010: `--ui-screenshot <screen>` loads the named document, settles, and writes a PNG.
- FR-011: `--ui-fixtures` supplies deterministic data for settings/world-select/gallery/photo
  so captures are reproducible.
- FR-012: Capture at the reference aspect (3840×1600 / 2.4:1).

### Hot reload (F3)
- FR-020: `UIHotReload` instantiated and pumped per frame behind a flag; RCSS/RML edits reload
  the active document within ~1s without restart.

### Backdrop (F4)
- FR-030: Pre-game menu bodies are transparent; a live scenic menu world renders behind them,
  blurred via `backdrop-filter`.
- FR-031: Pause uses a light frosted scrim (not near-black) over the frozen world.

### Thumbnails (F5)
- FR-040: World save emits `worlds/<id>/thumbnail.png`; world-select cards show it via `<img>`.
- FR-041: Photo capture emits PNG; gallery is populated from `photos/*.png` via `<img>`.

### Per-screen fidelity (loop)
- FR-050: Each of the 8 screens passes adversarial critic consensus against its reference on
  composition, typography/tracking, palette/glass, and polish lenses.

## Non-Functional Requirements
- NFR-001: UI pass holds the frame budget (per-frame native-res backdrop blur freezes/caches
  on menu entry if needed); existing render perf gate stays green.
- NFR-002: Determinism baselines and existing visual gates stay green; re-bless intentionally
  changed UI baselines.
- NFR-003: Build with ucrt64 prepended on PATH; build the tree under test.

## Acceptance Criteria
- [ ] AC-001: `--ui-screenshot settings` shows real panel blur + soft shadow (not solid fills);
  overlay not black/mis-scaled; world + ImGui still render correctly.
- [ ] AC-002: All 8 `--ui-screenshot <screen> --ui-fixtures` captures pass critic consensus vs
  `references/ui/<screen>.png`.
- [ ] AC-003: Main menu shows a live, slowly drifting, blurred golden-hour world behind the nav.
- [ ] AC-004: Saving a world shows a real snapshot on its card; capturing a photo shows it in
  the gallery grid.
- [ ] AC-005: Editing a color in `game_theme.rcss` updates the running UI within ~1s.

## Open Questions
- Exact 6.1 stylesheet cache-clear API for hot reload (`Rml::Factory::ClearStyleSheetCache()`
  vs `StyleSheetFactory`) — confirm against fetched headers.
- Which GL-loader macro the fetched GL3 backend expects (glad vs bundled gl3w).
- Scenic "menu world" preset choice for the backdrop.
