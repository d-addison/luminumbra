# ui visual scenarios

Generated from [the catalog](visual-catalog.json). [Roster](visual-catalog-roster.md) · [shared packet contract](visual-catalog.md).

## U01

**Main menu**

Create a compelling legible entry screen with finished composition, hierarchy, focus and notification states.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: Refresh maintained PNG from actual UI; normal/keyboard-focus/notification variants, small and standard windows. Existing docs PNG is historical until requalified.

Source links:

- [data/ui/main_menu.rml](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/data/ui/main_menu.rml)
- [test/ui/ui_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/ui/ui_smoke_test.cpp)
- [test/ui/ui_page_load_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/ui/ui_page_load_test.cpp)

Required variants:

- normal
- keyboard-focus
- notification
- small-window
- standard-window

Automated temporal evidence: **no separate motion claim**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S5: Actual UI states and disposable public save/photo fixtures.

Capture: **existing_subset**. One default page image each at 800 x 600, written to the build-defined artifact directory. Missing GL may skip. This does not cover the required interactions or variants.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<build>/bin/ui_smoke_test --gtest_filter=UiSmokeTest.CapturesMaintainedMenuScreenshots
```

Acceptance checks:

- Create a compelling legible entry screen with finished composition, hierarchy, focus and notification states.
- Refresh maintained PNG from actual UI; normal/keyboard-focus/notification variants, small and standard windows. Existing docs PNG is historical until requalified.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Maintained screenshot test captures the default main menu only; full focus/notification/scale variants are missing.

Retained evidence:

- None qualified for this catalog row.

Next action: Refresh main-menu normal/focus/notification states at explicit display scales; submit clean image with typography/contrast and navigation evidence.

## U02

**World creation and diorama**

Make controls and preview feel coherent; clearly communicate seed/preset changes and keep Create reachable.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: Empty/valid/invalid input, every authored preset preview, orbit/weather/time changes, debounce latest-wins, small-window scroll and create action. Use worldgen_preview_test.cpp and ui_page_load_test.cpp.

Source links:

- [data/ui/world_creation.rml](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/data/ui/world_creation.rml)
- [test/ui/ui_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/ui/ui_smoke_test.cpp)
- [test/ui/ui_page_load_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/ui/ui_page_load_test.cpp)

Required variants:

- empty
- valid
- invalid
- every-authored-preset
- orbit
- weather
- time
- latest-wins
- small-window
- create

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S5: Actual UI states and disposable public save/photo fixtures.

Capture: **existing_subset**. One default page image each at 800 x 600, written to the build-defined artifact directory. Missing GL may skip. This does not cover the required interactions or variants.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<build>/bin/ui_smoke_test --gtest_filter=UiSmokeTest.CapturesMaintainedMenuScreenshots
```

Acceptance checks:

- Make controls and preview feel coherent; clearly communicate seed/preset changes and keep Create reachable.
- Empty/valid/invalid input, every authored preset preview, orbit/weather/time changes, debounce latest-wins, small-window scroll and create action. Use worldgen_preview_test.cpp and ui_page_load_test.cpp.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Maintained screenshot test captures a default creation page; all preset/input/debounce/scale/interactive variants remain unqualified.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture creation form and actual preview for every preset with valid/invalid inputs and small-window action visibility; measure preview changes and latest-wins behavior.

## U03

**World selection**

Make populated, empty, validating and refused saves understandable with responsive layout.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: Empty/populated/selected/pending/corrupt/missing/obsolete/future/error states; actual IDs, refusal reason and live navigation proof. Existing maintained screenshot covers only one state.

Source links:

- [data/ui/world_selection.rml](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/data/ui/world_selection.rml)
- [test/ui/ui_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/ui/ui_smoke_test.cpp)
- [test/ui/ui_page_load_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/ui/ui_page_load_test.cpp)

Required variants:

- empty
- populated
- selected
- pending
- corrupt
- missing
- obsolete
- future
- error

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S5: Actual UI states and disposable public save/photo fixtures.

Capture: **existing_subset**. One default page image each at 800 x 600, written to the build-defined artifact directory. Missing GL may skip. This does not cover the required interactions or variants.

Arguments are a command template, not a newly executed run. Replace placeholders with owned native paths and a fresh output directory; reserve the native slot first.

```text
<build>/bin/ui_smoke_test --gtest_filter=UiSmokeTest.CapturesMaintainedMenuScreenshots
```

Acceptance checks:

- Make populated, empty, validating and refused saves understandable with responsive layout.
- Empty/populated/selected/pending/corrupt/missing/obsolete/future/error states; actual IDs, refusal reason and live navigation proof. Existing maintained screenshot covers only one state.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Maintained screenshot test captures one default selection page; refused/corrupt/future save and interaction variants remain unqualified.

Retained evidence:

- None qualified for this catalog row.

Next action: Build disposable empty/populated/pending/refused save states; capture actual messages and selection behavior without private user saves.

## U04

**Settings**

Make setting groups, current values, disabled controls and keyboard focus visually clear.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: All sections/tabs, enabled and audio-disabled variants, roundtrip values and dependent controls; screenshots at supported scale profiles.

Source links:

- [data/ui/settings.rml](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/data/ui/settings.rml)
- [test/ui/ui_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/ui/ui_smoke_test.cpp)
- [test/ui/ui_page_load_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/ui/ui_page_load_test.cpp)

Required variants:

- all-sections
- enabled-audio
- disabled-audio
- roundtrip
- focus
- supported-scales

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S5: Actual UI states and disposable public save/photo fixtures.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Make setting groups, current values, disabled controls and keyboard focus visually clear.
- All sections/tabs, enabled and audio-disabled variants, roundtrip values and dependent controls; screenshots at supported scale profiles.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Authored page and load assertions exist. Dedicated retained screenshots for every required interaction/state are missing.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture all authored settings sections and disabled-audio state with actual roundtrip values, focus and scale checks.

## U05

**Pause**

Produce a legible in-game overlay that preserves scene context and clear action hierarchy.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: Actual paused scene, focus/hover, resume/settings/save/quit transitions supported by authored callbacks.

Source links:

- [data/ui/pause.rml](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/data/ui/pause.rml)
- [test/ui/ui_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/ui/ui_smoke_test.cpp)
- [test/ui/ui_page_load_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/ui/ui_page_load_test.cpp)

Required variants:

- paused
- hover
- focus
- resume
- settings
- save
- quit

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S5: Actual UI states and disposable public save/photo fixtures.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Produce a legible in-game overlay that preserves scene context and clear action hierarchy.
- Actual paused scene, focus/hover, resume/settings/save/quit transitions supported by authored callbacks.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Authored page and load assertions exist. Dedicated retained screenshots for every required interaction/state are missing.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture real paused gameplay and action/focus states; preserve visible world context and verify resume/settings/save transitions.

## U06

**Photo mode**

Show a usable photographic composition workflow with legible camera controls and genuine output.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Control overlay and clean saved image, exposure/FOV/filter variants, before/after actual framebuffer; wire to photo-mode tests and emitted file identity.

Source links:

- [data/ui/photo_mode.rml](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/data/ui/photo_mode.rml)
- [test/ui/ui_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/ui/ui_smoke_test.cpp)
- [test/ui/ui_page_load_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/ui/ui_page_load_test.cpp)

Required variants:

- control-overlay
- clean-output
- exposure
- fov
- filter

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S5: Actual UI states and disposable public save/photo fixtures.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Show a usable photographic composition workflow with legible camera controls and genuine output.
- Control overlay and clean saved image, exposure/FOV/filter variants, before/after actual framebuffer; wire to photo-mode tests and emitted file identity.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Authored page and load assertions exist. Dedicated retained screenshots for every required interaction/state are missing.

Retained evidence:

- None qualified for this catalog row.

Next action: Trace actual photo-output path, capture control overlay and resulting clean image with camera/exposure settings; verify file and pixel identity.

## U07

**Gallery**

Make browsing actual captured photos attractive and readable with sensible empty/failure states.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Empty/populated/selected/back navigation, thumbnails tied to actual full images; loading/missing states if implemented.

Source links:

- [data/ui/gallery.rml](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/data/ui/gallery.rml)
- [test/ui/ui_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/ui/ui_smoke_test.cpp)
- [test/ui/ui_page_load_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/ui/ui_page_load_test.cpp)

Required variants:

- empty
- populated
- selected
- back
- missing-image

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S5: Actual UI states and disposable public save/photo fixtures.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Make browsing actual captured photos attractive and readable with sensible empty/failure states.
- Empty/populated/selected/back navigation, thumbnails tied to actual full images; loading/missing states if implemented.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Authored page and load assertions exist. Dedicated retained screenshots for every required interaction/state are missing.

Retained evidence:

- None qualified for this catalog row.

Next action: Populate gallery only from captured public fixtures, record empty/populated/selected states and tie each thumbnail to full original.

## U08

**HUD**

Keep vital gameplay information legible over bright, dark and busy backgrounds without obscuring play.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Actual gameplay capture with each implemented HUD component/state; bright/day/night and scaled-window variants, event/state assertions.

Source links:

- [data/ui/hud.rml](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/data/ui/hud.rml)
- [test/ui/ui_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/ui/ui_smoke_test.cpp)
- [test/ui/ui_page_load_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/ui/ui_page_load_test.cpp)

Required variants:

- each-implemented-component
- bright
- night
- busy
- scaled-window

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S5: Actual UI states and disposable public save/photo fixtures.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Keep vital gameplay information legible over bright, dark and busy backgrounds without obscuring play.
- Actual gameplay capture with each implemented HUD component/state; bright/day/night and scaled-window variants, event/state assertions.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Authored page and load assertions exist. Dedicated retained screenshots for every required interaction/state are missing.

Retained evidence:

- None qualified for this catalog row.

Next action: Enumerate implemented HUD states, then capture bright/dark/busy contexts with triggering game state and readable scale variants.

## U09

**Codex**

Show well-organized readable discovery content and meaningful locked/unlocked states.

Origin: original. Priority: P2. Execution: **not_captured**. Approval: **pending**.

Fixture: Populated and undiscovered states, actual species/content identity, scrolling/focus and opening/closing behavior.

Source links:

- [data/ui/codex.rml](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/data/ui/codex.rml)
- [test/ui/ui_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/ui/ui_smoke_test.cpp)
- [test/ui/ui_page_load_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/ui/ui_page_load_test.cpp)

Required variants:

- undiscovered
- discovered
- scroll
- focus
- open-close

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S5: Actual UI states and disposable public save/photo fixtures.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Show well-organized readable discovery content and meaningful locked/unlocked states.
- Populated and undiscovered states, actual species/content identity, scrolling/focus and opening/closing behavior.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: Authored page and load assertions exist. Dedicated retained screenshots for every required interaction/state are missing.

Retained evidence:

- None qualified for this catalog row.

Next action: Capture actual undiscovered/discovered species entries and navigation/scroll state from public content; separate live data from decorative examples.

## U10

**Shared window/modal components**

Ensure shared chrome, dialogs and destructive confirmations are consistent and accessible.

Origin: original. Priority: P1. Execution: **not_captured**. Approval: **pending**.

Fixture: Discover whether window.rml is standalone or template; capture actual consumers for rename/delete/overwrite/cancel/focus restoration rather than inventing a screen.

Source links:

- [data/ui/window.rml](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/data/ui/window.rml)
- [test/ui/ui_smoke_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/ui/ui_smoke_test.cpp)
- [test/ui/ui_page_load_test.cpp](https://github.com/d-addison/luminumbra/blob/945ecd295835a838eed0169ff709e7aabc134fa1/test/ui/ui_page_load_test.cpp)

Required variants:

- actual-consumers
- rename
- delete
- overwrite
- cancel
- focus-restoration

Automated temporal evidence: **required**.

Implementation dependencies:

- S1: Immutable scenario packets, capture validation and explicit approval records.
- S5: Actual UI states and disposable public save/photo fixtures.

Capture: **missing**. No reviewed command retaining the complete required image/temporal packet has been established. Supporting source tests do not fill this gap.

Acceptance checks:

- Ensure shared chrome, dialogs and destructive confirmations are consistent and accessible.
- Discover whether window.rml is standalone or template; capture actual consumers for rename/delete/overwrite/cancel/focus restoration rather than inventing a screen.
- Meet the shared packet contract and preserve independent regression checks; each listed variant is required.

Source reconciliation at `945ecd295835a838eed0169ff709e7aabc134fa1`: window.rml is a shared document; map actual dialog consumers and capture focus restoration. No standalone page is invented.

Retained evidence:

- None qualified for this catalog row.

Next action: Map window.rml to actual consumers, capture rename/delete/overwrite/cancel dialogs and focus restoration instead of inventing a standalone screen.
