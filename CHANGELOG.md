# Changelog

## v0.3.1 — in preparation

- Development continues on top of v0.3.0.

## v0.3.0 — 2026-10-16

- Enable all shipping simulation/render SystemConfig gates in compiled and
  packaged defaults, plus rendering and ordinary client audio defaults. Explicit
  false config values, renderer overrides and saved user settings remain honored;
  debug toggles, Vulkan, Steam and GNS remain opt-in.
- Default server autosave to 9000 ticks (five minutes at 30 Hz); zero disables it.
  Capture and determinism fixtures explicitly retain 30-tick autosaves.
- Default replication to acknowledged deltas and radius-3 chunk AOI, with explicit
  full-snapshot and AOI opt-outs.
- Bind TCP admission, lockstep and replay to effective simulation configuration
  and resolved preset content, including exact biome/structure dependency bytes
  consumed from an immutable snapshot. Runtime/soak TCP Hello has a 10-second
  deadline and releases expired slots (issue #315). Lockstep protocol and LREC1
  replay layout are version 3; older layouts are refused.
- Add revision-7 preset defaults while retaining revision-6 omission semantics.
  Current-format saves lacking embedded presets for the eight packaged
  names resolve to frozen revision-6 copies; new saves embed resolved choices.
- Check shipping simulation budgets for default, archipelago and mountains with
  complete stage/total coverage; historical gates-off workloads remain explicit.
- Retain v0.3.1 scope for regional plants/water continuation (issue #242) and FW S8
  saved-world far authority. Production far authority remains fixture-only with
  an explicit renderer unavailable/refusal diagnostic. Regional continuation
  performance/default enablement remains v0.4.0 (issue #320).

Current-candidate results are pending native verification. Source landing does
not establish native render, save/load, networking or audible qualification.
See [release notes](docs/release-notes-v0.3.0.md) for the full included scope,
obsolete-world format break and publication gate.
