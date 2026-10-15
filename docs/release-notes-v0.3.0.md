# Luminumbra v0.3.0 release notes

Release preparation is in progress. v0.3.0 contains the integrated `devel`
implementation, with every shipping feature enabled by default and dedicated-server
autosave every five minutes. Current-candidate native acceptance is pending; the
owner runs the release smoke on Windows.

## Included in v0.3.0

- Procedural terrain and rendering corrections, deterministic simulation repairs,
  persisted clock, active-region ledger and scheduler foundations, edited-content
  digest and scheduler-fault handling.
- Five-tier volumetric generation and meshing, build queues, residency and upload
  accounting, renderer integration, far-water submission, fog, shadow exclusion
  and horizon diagnostics. Ladder is requested by default. Normal presets lack
  immutable authority and still refuse Ladder with an effective Legacy fallback
  (roughly 3 km range and a 3,200 m far plane). This is not native 16 km acceptance.
- Far edit-overlay codecs/writers and bake derivation. The committed-generation
  edited-SDF producer and FW S8 consumer are planned for v0.3.1.
- Background saved-world catalog validation, lifecycle and refusal fixtures,
  synthetic saves and receipt validation. The dedicated server autosaves every
  five minutes.
- Compiled-prefab consumption, installed static preview, persistent viewport
  hosting, Blender viewport integration and bounded authoring validation.
- Source-release inventory and independent verification tooling, crash-capture
  tooling, draft CI and batched devel CI policies.

Experimental audio playback is outside the shipping feature set and retains its
[separate release policy](audio-release-policy.md). Debug toggles and non-shipping
Vulkan, Steam and GNS backends remain opt-in. Source implementation and supporting
fixtures do not establish native qualification or visual approval.

## Compatibility break

Create a new world: obsolete saved worlds cannot be opened and migration is not
supported, including metadata-only old worlds. Current saves use LMR1 container
v2, manifest schema `luminumbra.persistence.world_manifest.v1` with
`container_version: 2`, and FSD2 far-LOD payload v3. Explicit terrain preset
`schema_rev` must be `6`. See the [format contract](engine-guide.md).

## Planned for v0.3.1

- Indexed authority durability/rehydration, durable completed work and ground-object
  continuation, durable regional fields/weather/scent/energy, and regional
  foliage/water continuation with host persistence. The DS S3–S7 durability stack
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

The v0.3.0 publication gate requires all required CI checks green on the release
commit, a signed source tag, independent release-download verification and the
owner Windows smoke: launch the client, create a world, edit, save, quit, reload
and confirm the edit persists; run the server smoke. No current-candidate native
pass is claimed. Expanded acceptance deferred to v0.3.1 is not a pass.

See the [acceptance record](v0.3-acceptance.md),
[implementation limits](distant-world-implementation-status.md) and
[roadmap](roadmap.md).
