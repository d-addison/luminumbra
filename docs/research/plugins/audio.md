# Audio tooling research

## Current state

This report distinguishes repository evidence from the task inventory. The task inventory
identifies miniaudio 0.11.22 behind `IAudioManager` with `MiniaudioManager` and
`NullAudioManager`, the `AudioPropagationSystem`, `EnvironmentalAudioSystem`,
`AudioSpatialCluster`, and `MixerModel`, 68 MP3 assets, and three audio design documents.
Those implementation and inventory claims were supplied to this task; the deliberately
narrow read scope did not include re-counting the tree or inspecting the concrete manager
and spatial-system implementations.

The inspected interface is already a useful seam. It exposes bank load/unload, event and
one-shot playback, music and ambient-loop control, listener transforms, event parameters,
and a master/music/SFX/ambient/events/UI bus hierarchy. Its comments consistently mark
audio and bus state as render-only rather than simulation state
([`IAudioManager.h`](../../../src/luminumbra_client/audio/IAudioManager.h)). That makes
pipeline and tooling improvements possible without replacing the game-facing API.

The two inspected banks are small, human-authored JSON documents. `music.bank.json` has a
bank-wide `streaming` flag and three looping 2D tracks. `sfx_main.bank.json` uses non-empty
file lists, per-event volume, optional pitch variation and random selection, 2D/3D flags,
and optional linear attenuation distances. It also contains underscore-prefixed human
notes and intentional asset reuse, so a schema must preserve loader-tolerated metadata
instead of blindly setting `additionalProperties: false`
([`music.bank.json`](../../../data/audio/music.bank.json),
[`sfx_main.bank.json`](../../../data/audio/sfx_main.bank.json)).

The current generator reads `tools/audio/sfx_manifest.json`, calls ElevenLabs with a key
held only in the environment, receives MP3, and either keeps MP3 or asks FFmpeg to
transcode according to the requested output extension. It checks that FFmpeg exists but
does not validate the authoring manifest, measure loudness, constrain channels/sample
rate, retain a lossless source master, produce provenance, or verify the decoded result
([`generate_sfx.ps1`](../../../tools/audio/generate_sfx.ps1)).

The audio handoff records an important build-specific constraint: the current executable
has no Vorbis decoder, Ogg files have failed silently, and shipped assets should therefore
be MP3 or WAV. This is consistent with miniaudio's upstream documentation: its built-in
decoders are WAV, MP3, and FLAC, while Vorbis requires a custom decoding backend
([project handoff](../../AUDIO-everything-maps-to-sound.md),
[miniaudio manual](https://miniaud.io/docs/manual/index.html#8.-Decoding)). The handoff
also says only the main SFX and music banks are loaded, the listener is updated in game,
and audio is kept client/render-only so scenario gates and `world_hash` are unaffected.
These are valuable contracts to preserve, but the report does not treat broader README
architecture claims as evidence.

### Gaps and constraints

- Bank syntax, cross-bank event uniqueness, asset existence, path safety, supported
  codecs, and conditional 3D fields are not checked before runtime.
- The generator's transcode is not an ingestion pipeline: output can vary in loudness,
  peak, layout, rate, encoder version, and metadata, and there is no machine-readable
  receipt connecting source, settings, and result.
- MP3 is runtime-compatible and space-efficient, but is lossy and may introduce padding
  or boundary artifacts that matter for tiny transients and seamless loops. WAV is
  exact and cheap to decode but larger. Ogg/Vorbis is not a policy option until the
  actual miniaudio build gains and ships a tested Vorbis backend on every platform.
- The JSON bank workflow has no sound-designer preview, transactional hot reload, or
  automated audible regression gate.
- The ambitious design direction is substantially ahead of the authoring toolchain.
  Replacing miniaudio would add capable authoring software, but would also replace a
  committed runtime subsystem and force a migration of bank, bus, streaming, spatial,
  lifecycle, platform, build, and licensing behavior.

## Candidate integrations

Effort assumes one engineer familiar with the codebase. `S` is 1–3 working days, `M` is
4–8 days, `L` is 2–4 weeks, and `XL` is more than 4 weeks, excluding sound-content work.

| Candidate | Delivery form | Effort | Risk | Recommendation |
|---|---|---:|---|---|
| 1. Bank contract validator | Standalone `audio-bank-validate` command plus a targeted Banso preflight step | S | Low | Build first |
| 2. Reproducible SFX/music ingestion | `audio-ingest` CLI/PowerShell entry point, policy file, lossless masters, cooked outputs, and JSON receipts | M | Medium | Build second |
| 3. DAW-native export bridge | DAW-agnostic export contract, optional REAPER template/ReaScript, and watched staging inbox | S–M | Low–medium | Build with ingestion |
| 4. Transactional bank hot reload | Development-only bank/asset watcher and reload console command behind `IAudioManager` | M | Medium | Build after validation |
| 5. Audio regression harness | Fixed offline render fixtures, canonical PCM artifacts, and metric/tolerance reports | M–L | Medium | Pilot after ingestion |
| 6. FMOD Studio runtime spike | Time-boxed alternate `IAudioManager` backend, sample bank, migration ADR, and cost/platform gate | L | High | Defer; spike only on proven need |
| 7. Wwise runtime spike | Time-boxed alternate `IAudioManager` backend, sample SoundBank, migration ADR, and cost/platform gate | XL | High | Defer; highest migration burden |

### 1. Bank contract validator

**Delivery form:** a repository-owned standalone validator invoked identically by a
developer, CI, and a targeted Banso step. The Banso step should validate only changed
audio manifests plus the loaded-bank set and return file/event/property diagnostics; it
should not run the game or a repository-wide verification. Use JSON Schema draft 2020-12
for structure and a small semantic pass for filesystem and cross-document rules. JSON
Schema validators take a schema and an instance and return validation results, while the
standard itself acknowledges that sufficiently complex formats commonly need structural
and semantic phases
([JSON Schema getting started](https://json-schema.org/learn/getting-started-step-by-step),
[structural versus semantic validation](https://json-schema.org/understanding-json-schema/about)).

The structural schema should cover:

- root `bank_id`, optional boolean `streaming`, and a non-empty `events` object;
- each event's non-empty string `files` array, finite `volume`, optional finite
  `pitch_variation`, known `strategy`, and boolean spatial flags;
- conditional numeric attenuation fields, with enums for known attenuation models;
- underscore-prefixed annotations such as `_placeholder_note`, while warning on unknown
  behavioral keys until the loader contract is deliberately frozen.

The semantic pass should reject missing files, absolute paths, `..` traversal, unsupported
extensions, duplicate event IDs across simultaneously loaded banks, empty/duplicate file
entries, contradictory 2D/3D flags, non-finite values, and `min_distance >= max_distance`
when both distances exist. It should warn—not immediately fail—when a 3D event omits an
attenuation policy because the inspected ambient entries currently do so. It should also
decode-probe every referenced asset using the same enabled decoder set as the shipped
runtime; FFmpeg success alone does not prove that the current miniaudio build can load it.

Adopt strictness in two phases: report the existing corpus first, then make zero-false-
positive rules blocking. The validator is render-data tooling and cannot affect
`world_hash`; nevertheless, any future autofix must remain opt-in and must never rewrite
simulation data or event ordering.

**Effort/risk:** S / low. The main risk is encoding assumptions from documentation rather
than the actual loader. Mitigate with fixtures derived from the two loaded banks and a
runtime-decoder smoke test.

### 2. Reproducible SFX/music ingestion

**Delivery form:** one `audio-ingest` entry point used for generated and human-authored
material. It consumes a manifest, reads immutable source masters from a staging area,
writes cooked runtime files to their declared bank paths, and emits a receipt containing
input SHA-256, tool versions, command/settings, duration, channels, sample rate, codec,
integrated/short-term loudness as applicable, true peak, and cooked SHA-256. Keep the
ElevenLabs call as an optional acquisition adapter; generation and cooking should be
separate stages so `-Force` never destroys the only source.

Recommended format policy:

| Use | Authoring/master | Current cooked format | Reason |
|---|---|---|---|
| Short one-shots, critical transients, seamless short loops | WAV, 24-bit PCM, project-standard sample rate | WAV when size budget permits | Exact boundaries, no lossy generation loss, cheap decode |
| Long music and ambience | WAV, 24-bit PCM | MP3 at a pinned quality setting | Current runtime support and materially smaller assets |
| Ogg/Vorbis | WAV master | Not allowed today | Current build lacks the decoder; enable only after a cross-platform decoder spike |
| Archival generated source | Original response plus WAV master | Never shipped directly unless policy permits | Preserves provenance and permits repeatable recooking |

Do not transcode MP3 to MP3 repeatedly. If an API supplies only MP3, archive those exact
bytes, decode once to the lossless working master, and make all later cooks from that
master. WAV growth should be tracked by a bank budget; if short-SFX size is unacceptable,
retain MP3 until a lossless compressed format such as FLAC is explicitly tested in the
actual build. Upstream miniaudio lists FLAC as built in, but repository runtime behavior
must win over upstream defaults.

For long material, use FFmpeg `loudnorm` in measured two-pass linear mode with pinned
FFmpeg builds. It supports EBU R128 integrated loudness, loudness range, and maximum true
peak targets in both single- and double-pass modes
([FFmpeg filter documentation](https://ffmpeg.org/ffmpeg-filters.html#loudnorm)). Store
targets by content family rather than imposing one value on the entire library: music,
ambience, UI, impacts, footsteps, and creature calls serve different mix roles. Calibrate
the actual numbers in an in-game listening scene; bank `volume` remains creative mix gain,
not a substitute for asset normalization.

Very short SFX should not be forced through an integrated-LUFS target that is unstable or
misleading for transients. For them, enforce true-peak headroom, duration/channel/rate
policy, silence/truncation checks, and a family-relative short-term or RMS range. Mono cues
intended for stereo playback also need deliberate measurement semantics; FFmpeg documents
a `dual_mono` compensation option. Fail ingestion on clipping, unexpected silence, wrong
layout, an unapproved extension, or a missing receipt.

**Effort/risk:** M / medium. Tool-version drift and audible changes from normalization are
the risks. Pin versions, preserve masters, support dry-run reports, and require an audition
before accepting large recooks.

### 3. DAW-native export bridge

**Delivery form:** a DAW-agnostic drop contract plus an optional REAPER starter project and
Lua ReaScript. Regions/stems export named event variants as 24-bit WAV plus a small sidecar
with event ID, family, loop intent, channel intent, and requested bank. A watched staging
inbox invokes `audio-ingest --dry-run`, presents validation/loudness differences, and cooks
only after explicit acceptance.

This offers the valuable part of middleware authoring—fast batch export and repeatable
naming—without adding a second runtime. REAPER is a practical optional adapter because it
supports queued/batch rendering and command-line batch conversion, and ReaScript can call
REAPER actions and most of its API
([REAPER capabilities](https://www.reaper.fm/about.php),
[ReaScript documentation](https://www.reaper.fm/sdk/reascript/reascript.php)). The core
contract must remain usable from any DAW so project assets do not depend on one editor.

The bridge should never write bank JSON directly. It submits a candidate export; the
validator/ingestion tools own paths, normalization, and manifest edits or patches. That
separation prevents a DAW script from silently breaking runtime data.

**Effort/risk:** S–M / low–medium. The chief risk is naming and region metadata drifting
between individual DAW projects. A single export template and receipt preview contain it.

### 4. Transactional bank hot reload

**Delivery form:** a development-only file watcher plus an explicit `audio.reload` console
command implemented behind the existing interface. Watch only loaded bank manifests and
their referenced assets. Debounce changes, validate first, load into a shadow bank, and
swap the event map only after every new resource is ready. On failure, keep the old bank
playing and report the exact event/file error. Defer disposal until active handles no
longer reference the old resources; bank unload/load in place is not safe enough without
confirming manager lifetime behavior.

Support three iteration levels: manifest-only parameter refresh; one-asset recook and
event refresh; full bank refresh when IDs or routing change. Preserve the current listener,
bus gains, music/ambient intent, and active-event policy across a successful swap. A small
overlay should show generation number, last reload time, and failures.

This feature must be compiled or configured out of release, headless, scenario, replay,
and gate runs. Watcher timing, file timestamps, bank generation, audio random selection,
and reload success must never enter simulation state, simulation PRNG streams, event
ordering, saves, networking, or `world_hash`. If any future gameplay system observes
audio completion or handles, the design must be re-reviewed because that would violate
the current render-only contract.

**Effort/risk:** M / medium. Resource lifetime and partially applied reloads are the hard
parts. Transactional shadow loading and a manual fallback command are mandatory.

### 5. Audio regression harness

**Delivery form:** a small offline fixture runner and report generator. Fixtures explicitly
select files/events, fixed listener/emitter transforms, bus gains, timestamps, and output
format (for example, stereo 48 kHz float mixed to a canonical WAV). Test asset decoding
and mix behavior separately. The miniaudio decoder is independent of an output device,
and its encoder can write WAV, so an offline path is compatible with the chosen runtime
library
([miniaudio decoding and encoding](https://miniaud.io/docs/manual/index.html#8.-Decoding)).

Use two comparison levels:

1. Exact hash only for canonical PCM produced by the same pinned toolchain. Hashing MP3
   bytes proves packaging identity, not audible identity; hashing decoded floats across
   compilers, CPUs, or decoder versions is too brittle for the primary gate.
2. Tolerance-based audible metrics as the durable gate: duration, silence ratio, sample
   peak/true peak, integrated or short-term loudness, channel count, DC offset, and a
   coarse spectral-distance measure. Include a rendered difference file when a threshold
   fails. Long material can use EBU R128; transient families need peak and relative-level
   thresholds.

Start with asset-level decode/loudness tests, then add five mix fixtures: 2D UI, one 3D
event at known distance, randomized multi-file event with an explicit fixture-owned clip
choice, ambient loop boundary, and music/SFX/bus gain interaction. Never seed or consume
the simulation RNG merely to stabilize audio. Offline fixtures must run outside the
simulation and their outputs must not be folded into `world_hash`; they are a separate
render regression contract.

**Effort/risk:** M–L / medium. Thresholds can be noisy and exact renders can be platform
sensitive. Pin the reference environment, keep metric tolerances explainable, and require
human audition for intentional baseline updates.

### 6. FMOD Studio runtime spike

**Delivery form:** a time-boxed branch implementing a representative alternate backend
behind `IAudioManager`, one UI event, one spatial randomized event, one looping ambience,
one music track, all six buses, parameter updates, bank build in CI, and a written
architecture/licensing/platform ADR. Do not ship both miniaudio and FMOD as simultaneous
owners of the same mix; the spike must measure a replacement path.

FMOD Studio provides DAW-like adaptive event authoring, built banks, live update, and a
profiler. Its content is played by the FMOD Engine Studio API, so adopting it is not merely
an editor integration
([FMOD Studio concepts](https://www.fmod.com/docs/2.03/studio/fmod-studio-concepts.html),
[FMOD Engine overview](https://www.fmod.com/docs/2.03/api/studio-api-bank.html)). The
existing interface can conceal basic calls, but semantics still need migration: IDs,
bank-version compatibility, handles, parameter lookup, 3D attributes, streaming, bus
hierarchy, active-loop lifetime, null/headless behavior, and the division of work between
FMOD spatialization and the existing propagation/environment/mixer systems.

Licensing is also a release input, not a later cleanup. FMOD's current game tiers are
budget-dependent and its distribution license, logo, support, and source access have
explicit conditions
([FMOD licensing](https://www.fmod.com/licensing)). Record the applicable tier and target
platforms in the ADR rather than assuming “free.”

**Effort/risk:** L / high for a useful spike; a production migration is likely larger.
Proceed only if a sound-design team demonstrates that adaptive authoring, live profiling,
and designer autonomy cannot be met by candidates 1–5.

### 7. Wwise runtime spike

**Delivery form:** the same representative replacement-backend spike and ADR as FMOD, but
using a Wwise project, generated SoundBank, event/game-parameter mapping, profiler session,
CI bank generation, and platform/license matrix.

Wwise offers deep event, mixing, propagation, SoundBank, and remote profiling workflows;
its profiler can connect to a running game and edit/mix while profiling
([Wwise profiling](https://www.audiokinetic.com/en/public-library/2024.1.8_8893/?id=profiling&source=Help)).
That strength overlaps most directly with luminumbra's existing spatial and mixer model,
which makes ownership boundaries and migration scope larger rather than smaller. The
engine-facing adapter still cannot avoid SDK build integration, generated bank lifecycle,
game-object registration, listener mapping, callbacks, headless behavior, and translating
or retiring existing spatial calculations.

Wwise licensing is per title and budget/platform dependent; its published game pricing
distinguishes Indie, Pro, Premium, and Platinum tiers and charges extra platforms in
non-Indie tiers
([Wwise game pricing](https://www.audiokinetic.com/pricing/for-games/)). Confirm the actual
commercial tier before any architectural commitment.

**Effort/risk:** XL / high. Wwise is justified only if the project needs its full authoring,
profiling, and spatial ecosystem and has dedicated audio engineering/design ownership.

## Ranking

1. **Bank contract validator.** Highest leverage and lowest risk. It turns silent runtime
   failures into precise preflight diagnostics and becomes the foundation for every other
   candidate.
2. **Reproducible ingestion pipeline.** Fixes irreversible generation/transcode behavior,
   establishes the MP3/WAV policy, and gives loudness and provenance measurable contracts.
3. **DAW-native export bridge.** Delivers fast designer iteration while retaining the
   existing runtime commitment; build it on the validator and ingestion command rather
   than as an independent pipeline.
4. **Transactional hot reload.** Materially improves in-game tuning, but only after invalid
   manifests and undecodable assets can be rejected before the swap.
5. **Audio regression harness.** Begin with decoder and metric smoke tests, then expand to
   offline mixes after cooking is reproducible. Prefer tolerance-based loudness/peak tests
   over a universal audio hash.
6. **FMOD Studio spike.** The more proportionate middleware evaluation if native tooling
   proves insufficient, but still a high-risk runtime replacement with licensing and
   spatial-ownership consequences.
7. **Wwise spike.** Most capable and most disruptive. Its overlap with existing spatial and
   mixer systems makes it the least suitable near-term choice.

The recommended delivery sequence is 1 → 2/3 → 4 → 5. At that point, measure designer
iteration time, defect escape rate, and unmet adaptive-audio needs for a milestone before
authorizing either middleware spike. All native tooling remains outside simulation. Any
proposal that changes event selection, timing, RNG use, or feeds audio state back into the
world must explicitly revalidate the `world_hash` contract before implementation.

### Sources consulted

Repository files analyzed:

- [`src/luminumbra_client/audio/IAudioManager.h`](../../../src/luminumbra_client/audio/IAudioManager.h)
- [`data/audio/music.bank.json`](../../../data/audio/music.bank.json)
- [`data/audio/sfx_main.bank.json`](../../../data/audio/sfx_main.bank.json)
- [`tools/audio/generate_sfx.ps1`](../../../tools/audio/generate_sfx.ps1)
- [`docs/AUDIO-everything-maps-to-sound.md`](../../AUDIO-everything-maps-to-sound.md)

Primary web documentation consulted (accessed 2026-08-21):

- [miniaudio manual](https://miniaud.io/docs/manual/index.html)
- [FFmpeg filters: `loudnorm`](https://ffmpeg.org/ffmpeg-filters.html#loudnorm)
- [JSON Schema specification and learning guide](https://json-schema.org/specification)
- [REAPER ReaScript documentation](https://www.reaper.fm/sdk/reascript/reascript.php)
- [FMOD Studio concepts and licensing](https://www.fmod.com/docs/2.03/studio/fmod-studio-concepts.html)
- [Wwise profiling and game pricing](https://www.audiokinetic.com/en/public-library/2024.1.8_8893/?id=profiling&source=Help)
