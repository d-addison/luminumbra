# Wave 1 Visual Gates

## Material Visual Gate Test

Task: T-EF-2-material-visual-gate-test

This artifact defines the visual gate used for Wave 1 material validation.

## Gate Scope

- Validate material rendering consistency across the Wave 1 engine frontier surface.
- Confirm that visual states remain legible under default, hover, active, disabled, and focused interactions.
- Verify that material contrast, elevation, border, shadow, and background treatments remain consistent across supported viewport sizes.

## Required Checks

1. Capture reference screenshots for each material state in the target viewport set.
2. Compare screenshots against the approved baseline with deterministic rendering settings.
3. Review any pixel differences for material regressions rather than expected content variation.
4. Record failures with the affected state, viewport, screenshot path, and observed material mismatch.

## Pass Criteria

- No unexpected material color, texture, opacity, elevation, border, or shadow changes are present.
- Interactive states are visually distinct and accessible.
- Layout changes do not crop, overlap, or obscure material surfaces.
- Screenshot comparison output is attached to the Wave 1 verification record.

## Failure Criteria

- Any unapproved material delta appears in the visual diff.
- Interactive material states are missing, indistinct, or inconsistent.
- Visual regressions reproduce across two consecutive gate runs.
- Required screenshots or comparison records are missing.

## Gate Result

Status: Pending execution

Owner: Engine Frontier Wave 1
