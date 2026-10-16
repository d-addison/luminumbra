# Luminumbra v0.3.0 release notes

Release preparation is in progress. v0.3.0 contains the integrated `devel`
implementation, with every shipping feature enabled by default and dedicated-server
autosave every five minutes. Current-candidate results are pending native verification; the
owner runs the release smoke on Windows.

## Included in v0.3.0

- Procedural terrain and rendering corrections, deterministic simulation repairs,
  persisted clock, active-region ledger and scheduler foundations, edited-content
  digest and scheduler-fault handling.
- Five-tier volumetric generation and meshing, build queues, residency and upload
  accounting, renderer integration, far-water submission, fog, shadow exclusion
  and horizon diagnostics. Ladder is requested by default. Normal presets lack
  immutable authority and still refuse Ladder with an effective Legacy fallback
  (roughly 3 km range and a 3,200 m far plane). Saved-world far production authority
  remains fixture-only until FW S8 (v0.3.1); the renderer reports an explicit
  unavailable/refusal diagnostic. Native 16 km results are pending native verification.
- Far edit-overlay codecs/writers and bake derivation. The committed-generation
  edited-SDF producer and FW S8 consumer are planned for v0.3.1.
- Background saved-world catalog validation, lifecycle and refusal fixtures,
  synthetic saves and receipt validation. The dedicated server autosaves every
  five minutes.
- Compiled-prefab consumption, installed static preview, persistent viewport
  hosting, Blender viewport integration and bounded authoring validation.
- Source-release inventory and independent verification tooling, crash-capture
  tooling, draft CI and batched devel CI policies.

Ordinary client audio playback is enabled; audible quality and event coverage
remain pending native verification under the [audio policy](audio-release-policy.md). Debug toggles and non-shipping
Vulkan, Steam and GNS backends remain opt-in. Source implementation and supporting
fixtures do not establish native qualification or visual approval.

## Shipping defaults and session compatibility

- All registered `sim.*` and `render.*` SystemConfig gates are on in compiled and
  packaged defaults. Explicit boolean false opts out. Declared gates without
  activation consumers do not introduce new simulation systems. Debug/diagnostic
  toggles and Vulkan, Steam and GNS paths remain opt-in; `flat_lands` retains its
  minimal preset identity.
- Rendering defaults include vsync, TAAU, tier-1 volumetrics, cave AO and scent
  decals. Ordinary client audio playback is enabled; captures use `--no-audio`.
  Explicit renderer overrides and saved user settings retain their choices.
- Server autosave defaults to 9000 ticks (five minutes at 30 Hz). Explicit zero
  disables it; smoke/determinism and capture fixtures explicitly use 30 ticks.
- Replication defaults to acknowledged deltas and radius-3 chunk AOI. Callers can
  restore full snapshots and disable chunk AOI; millimetre AOI remains disabled
  by default.
- Effective session SystemConfig identity is separate from the legacy config
  sub-hash and excludes presentation settings. TCP admission, lockstep Hello and
  replay headers bind effective simulation rules and resolved preset content.
  Runtime/soak TCP slots disconnect peers without a complete matching Hello at
  the 10-second deadline and release their slots (issue #315). GNS/Steam identity admission
  remains planned for v0.3.1.
- Preset content identity covers canonical resolved JSON plus a sorted manifest
  of logical paths and exact-byte hashes for resolved biome tables, structure
  placements and pieces. Generation consumes the same immutable dependency
  snapshot. Optional absent/rejected inputs have distinct markers; present
  unreadable inputs refuse. See the [identity contract](development/system-config-identity.md).
- Shipping-default simulation-budget capture checks all stages and totals for
  default, archipelago and mountains with `--require-complete`. Historical
  gates-off workloads remain explicit; shipping native results are pending native
  verification.

## Compatibility break

Create a new world: obsolete saved worlds cannot be opened and migration is not
supported, including metadata-only old worlds. Current saves use LMR1 container
v2, manifest schema `luminumbra.persistence.world_manifest.v1` with
`container_version: 2`, and FSD2 far-LOD payload v3. Explicit terrain preset
`schema_rev: 7` enables versioned missing-field defaults; revision `6` and
unversioned presets retain historical omission semantics. Current-format saves
without embedded presets that name one of the eight packaged presets
resolve to frozen revision-6 copies in `worlds/atlas/presets-rev6/`, preserving
terrain choices. Other named presets retain their previous resolution and
validation; new saves embed resolved presets. This fallback does not reopen
obsolete world formats. Lockstep protocol and LREC1 replay layout are version 3;
versions 1 and 2 are refused because they lack resolved-content identity.
See the [format contract](engine-guide.md).

## Planned for v0.3.1

- Indexed authority durability/rehydration, durable completed work and ground-object
  continuation, durable regional fields/weather/scent/energy, and regional
  foliage/water continuation with host persistence (issue #242). The DS S3–S7 durability stack
  is planned work and is not shipped in v0.3.0.
- Strict distant policy and distant creatures (S8), plus remaining S9 closure.
- The committed-generation edited-SDF producer and FW S8 consumer.
- The render qualification tooling stack, complete native render qualification,
  acceptance-matrix completeness, authoritative ground/high/cave-air poses and
  fixed-view workloads, cave-walk and edited-world traversal producers, the ladder
  horizon gate, and outstanding acceptance capture/pinning.
- The developer console, asymmetric day/night and the calendar-relative lunar cycle.
- GNS/Steam admission identity and the MG2 E4 candidate directory.

Regional continuation performance work and default enablement are planned for
v0.4.0 as [issue #320](https://github.com/d-addison/luminumbra/issues/320).

## Pending release evidence

The v0.3.0 publication gate is the full list in the
[acceptance record](v0.3-acceptance.md):

- all required CI checks green on the release commit, with strict CI promotion;
- final revision checks and applicable native/manual coverage;
- the 120-second UCRT64 guard;
- server-load qualification;
- the paired performance gates in [performance](performance.md);
- independent release verification;
- signed source-only publication and independent release-download verification;
- the owner Windows smoke: launch the client, create a world, edit, save, quit,
  reload and confirm the edit persists, then run the server smoke.

No current-candidate native pass is claimed. Expanded acceptance deferred to v0.3.1
is not a pass.

See the [acceptance record](v0.3-acceptance.md),
[implementation limits](distant-world-implementation-status.md) and
[roadmap](roadmap.md).
