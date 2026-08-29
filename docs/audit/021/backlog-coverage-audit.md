# Spec 021 — Backlog Coverage Audit and Expansion Handoff (2026-07-11)

| Field | Snapshot |
| --- | --- |
| Date | 2026-07-11 |
| Repository | <code>luminumbra</code> |
| Branch | <code>feat/polyglot-audit-roadmap</code> |
| HEAD | <code>cdfd0a816d939c2c835c02b960d36986d380904d</code> |
| Primary backlog | <code>docs/audit/021/backlog.json</code> |
| Primary ranking | <code>docs/audit/021/priority-ranking.md</code> |
| Workflow authority | <code>docs/audit/021/orchestration-plan.md</code> |

## Purpose and authority

This document is the repository handoff for the 2026-07-11 review of Luminumbra's code,
specifications, backlog, proof artifacts, feature wiring, and product-visible behavior. It
records what was surfaced, what evidence supports each finding, and how the backlog should
be repaired before implementation resumes.

This is an audit handoff, not the repaired canonical backlog. At the time this handoff was
written:

- <code>backlog.json</code> remains the formal item-detail source, and
  <code>priority-ranking.md</code> remains the only formal ranking until the accepted
  corrections are integrated. This handoff must not be counted as a second backlog.
- <code>orchestration-plan.md</code> remains the current route/workflow authority until its
  waves are deliberately reconciled with the repaired dependencies.
- <code>backlog.json</code>, <code>priority-ranking.md</code>, the pillar documents, and
  <code>validate_backlog.py</code> had not yet been corrected from this review.
- No Forge command was invoked. The owner explicitly asked that Forge be ignored while it
  is being prepared.
- Existing Forge reports were read as evidence only. Some are ignored artifacts and are
  not, by themselves, durable closure records.
- The repository was already materially dirty. Tracked edits and numerous untracked
  assets/artifacts belong to the owner and must be preserved. Do not clean, reset, restore,
  or broadly stage the worktree as part of backlog repair. At inspection, tracked changes
  included <code>.forge/config.yaml</code>, <code>.gitignore</code>, and
  <code>imgui.ini</code>, alongside many untracked Forge artifacts, captures, references,
  models, and vendor files.
- This handoff itself was created as a new untracked file. It becomes durable repository
  evidence only when an integrator intentionally stages and commits it; this review does
  not implicitly authorize a commit.

The next operator should treat this handoff as the controlling review input until its
accepted findings have been folded into the canonical backlog and ranking.

## Current Spec 021 artifact map

| Artifact | Role |
| --- | --- |
| <code>docs/specs/021-engine-framework-audit-charter/spec.md</code> | Charter, canonical taxonomy, and acceptance contract. |
| <code>docs/audit/021/pillar-*.md</code> | Twelve original pillar critiques and evidence inventories. |
| <code>docs/audit/021/backlog.json</code> | Formal item records and proving signals. |
| <code>docs/audit/021/validate_backlog.py</code> | Current structural validator. |
| <code>docs/audit/021/priority-ranking.md</code> | Single formal historical/active ranking, currently stale. |
| <code>docs/audit/021/gpu-modernization-plan.md</code> | GPU-specific decomposition and dependencies. |
| <code>docs/audit/021/orchestration-plan.md</code> | Existing workflow and route authority. |
| This document | Candidate corrections, expansion findings, sequencing changes, and integration checklist. |

## Owner intent and deferred orchestration

The broader owner request behind this review is not merely to make tests green. It is to
audit, critique, tune, and finish every meaningful framework area, system, mechanic, and
player journey, with particular emphasis on:

- Visual fidelity and art-direction credibility.
- Full end-user functionality rather than isolated substrate.
- Production feature wiring and intended-profile activation.
- Cross-system integration, persistence, networking, and reachability.
- Repeated visual/UX tuning and “massaging” after nominal implementation.
- Running the real game and judging the resulting experience, not stopping at headless
  gates.

The owner originally requested Forge orchestration and then explicitly paused Forge while
it is being prepared. The requested future orchestration shape was corrected to 27
subagent roles/wave participants, with 5.6-sol HXhigh-style reasoning/review roles for
investigation and critique and High-style coding roles for bounded implementation. Treat
those names as routing intent to be mapped to the actual Forge model/profile vocabulary
when Forge is ready; do not invent or execute that configuration early.

When orchestration resumes, it should consume the repaired backlog, not the stale 190-item
ranking. Every coding assignment should be preceded or paired with an independent
requirements/evidence critique, and every completion should return through integration,
real-game, visual, and acceptance review. The requested orchestrator prompt/workflow and a
fresh interactive game run remain deferred deliverables; neither was produced by this
backlog-only pass.

## Executive verdict

The current backlog is not safe to execute literally. It is useful historical material,
but it is stale, contains false-green completions, conflates framework substrate with
shipped behavior, double-counts several GPU findings, and omits multiple product-critical
systems.

The file currently reports:

| State | Current file | Post-digest atomic target |
| --- | ---: | ---: |
| Done | 160 | 145 |
| Todo | 30 | 26 |
| In progress | 0 | 19 |
| Total existing records | 190 | 190 |

The strict correction is calculated before adding any newly discovered work:

- Todo to done during atomic repair: <code>SHIELD-08</code>; <code>OPS-09</code> is
  closure-ready but must receive its tracked durable digest in the same change.
- Todo to in progress: <code>UI-09</code>, <code>OPS-16</code>.
- Done to in progress: <code>AETHER-07</code>, <code>AETHER-08</code>,
  <code>AETHER-10</code>, <code>AETHER-11</code>, <code>AETHER-12</code>,
  <code>ATMO-12</code>, <code>GPU-P01</code>, <code>GPU-P02</code>,
  <code>GPU-P04</code>, <code>GPU-03</code>, <code>GPU-P05</code>,
  <code>GPU-09</code>, <code>GPU-P09</code>, <code>GPU-07</code>,
  <code>GPU-14</code>, <code>NET-06</code>, and <code>NET-11</code>.

The 145 done / 26 todo / 19 in-progress ledger is the target after OPS-09 and its tracked
digest land atomically. Before that digest exists, the truthful interim correction is
144 done / 27 todo / 19 in progress.

There are two other useful counts, and they must not be confused with the strict ledger:

- Merely closing <code>SHIELD-08</code> and <code>OPS-09</code> would leave 28 nominal
  todos.
- Four of those nominal todos are duplicate or umbrella GPU records, leaving 24 canonical
  legacy todo deliverables before newly omitted work is added.

The eventual total will exceed 190 unless the repair first merges or supersedes duplicates.
Do not publish a new completion percentage until status correction, deduplication, and
missing-item insertion are complete.

## Spec-to-backlog coverage matrix

Counts below describe the current JSON before correction. “Present” does not mean the spec
is fully delivered; the disposition column records the truth gap found in this review.

| Spec | Topic | Current JSON mapping | Audit disposition |
| --- | --- | --- | --- |
| 001 | Cinematic RmlUi overhaul | 4 records: 2 done, 2 todo | <code>UI-06</code> real save/load and <code>UI-08</code> final ultrawide review remain open. |
| 002 | Deferred create-world flow | 6 records, all done | No specific status correction in this pass; complete persistence/load proof still constrains the product journey. |
| 003 | Deferred worldgen/water render | 1 record, done | Historical mapping exists; water visual coherence remains missing cross-cutting work. |
| 004 | Performance/600 FPS premise | No explicit mapping | Original premise was rescoped; absolute floor remains unblessed. Add <code>OPS-21</code>. |
| 005 | Emergent ecology AI | 2 records, both done | Framework coverage exists; authoritative normal-play wildlife is still missing under <code>INSTINCT-16</code>. |
| 006 | Living-world foliage/farming | 2 records: 1 done, 1 todo | Split FOLIAGE-07 correctness from scale; add or merge measurable visual polish under <code>FOLIAGE-12</code>. |
| 007 | Particle system | No explicit mapping | Held blocker is stale because the motion-vector MRT now exists. Add <code>RENDER-23</code>. |
| 008 | GPU-locked performance landables | No explicit mapping | Acceptance remains unchecked despite apparently landed components. Add <code>OPS-24</code> trace/reconciliation. |
| 009 | Flowing water/terraforming | 7 records, all done | Technical work is mapped; subjective water/shore/weather coherence remains under <code>WATER-18</code>. |
| 010 | Finite hydrology | 3 records, all done | No specific status correction in this pass. |
| 011 | Living creatures/daily life | 8 records: 7 done, 1 todo | <code>INSTINCT-14</code> remains; authoritative wildlife and distinct species visuals require <code>INSTINCT-16/17</code>. |
| 012 | Photography depth | No explicit mapping | Partial controls/metadata landed; histogram/focus and end-to-end proof remain. Add <code>UI-16</code>. |
| 013 | Points of interest | 1 done record | Only <code>RENDER-19</code> is mapped; phases 2 and 3 need <code>SHIELD-19/20</code>. |
| 014 | RHI/Vulkan/DX12 migration | 18 records: 8 done, 10 todo | Multiple done claims are scaffold/surrogate proof, and several records duplicate their implementation tasks. Reopen and deduplicate as specified below. |
| 015 | Atmospheric lighting/colored glass | 13 records, all done | No broad reopen in this pass; palette/lighting credibility still needs <code>RENDER-21/22</code>. |
| 016 | Render framework/frame graph | 12 records, all done | <code>GPU-P01</code> readiness is source-presence lint; architecture claims also need truth repair. |
| 017 | Concurrency/async execution | 13 records across 017/017-A/017-B/017-D, all done | No broad status correction in this pass; SHIELD-08 has separate ratified closure evidence. |
| 018 | Determinism hardening | 9 records across 018/018-B, all done | Spec, matrix, and README disagree about cross-build/platform hashes. Add <code>OPS-17</code>. |
| 019 | Networking scale-out | 12 records: 10 done, 2 todo | Reopen/split <code>NET-06</code> and <code>NET-11</code>; add real client host/join <code>NET-14</code>. |
| 020 | Build/test/operability/config | 9 records, all done | Packaging, intended-profile truth, hostile-input hardening, and lifecycle soak remain missing. |
| 021 | Engine-framework audit charter | 2 records, both done | <code>GPU-14</code> overstates Nsight proof; the charter backlog/ranking itself is stale. |
| 022 | Celestial-body seam | No explicit mapping | Tier 2 is charted, not built. Add <code>ATMO-16</code>. |
| 023 | Live shader authoring | No explicit mapping | Data-driven pass/material execution is charted, not built. Add <code>RENDER-24</code>. |
| 024 | Aether field completion | No explicit mapping | Current Aether records use spec “new”; the spec is draft, activation is off, and real participants are absent. Add <code>AETHER-13/14/15</code>. |

Sixty-six records currently use the catch-all spec value <code>new</code>. That category is
too broad to preserve specification traceability and is where several false greens and
unmapped product claims accumulated.

## Definition of done that this audit applies

A source file, class, shader, setter, configuration flag, test double, or isolated test is
evidence of implementation. It is not automatically evidence of delivery.

An item may be marked done only when the full claim satisfies all applicable dimensions:

| Dimension | Question |
| --- | --- |
| Implementation | Does the required behavior exist beyond a stub or surrogate? |
| Production wiring | Does the normal client/server/session path construct and call it? |
| Activation | Is the shipping profile or intended configuration able to enable it? |
| Reachability | Can a player, server operator, or authored content journey reach it? |
| Persistence/replication | Does authoritative state survive and synchronize where required? |
| Proof | Does current, non-skipped evidence exercise the claimed production behavior? |
| Product quality | For visual/audio/UX claims, has the result passed an appropriate human-facing review? |

Recommended machine-readable fields for the repaired backlog are:

~~~json
{
  "implementation": "absent | scaffold | partial | complete",
  "wiring": "none | test_only | production_dormant | production_active",
  "activation": "disabled | opt_in | shipping_default",
  "reachability": "unreachable | config_only | operator_reachable | user_reachable",
  "proof": "none | source | unit | integration | current_e2e | human_approved",
  "delivery_status": "todo | in-progress | blocked | done | superseded"
}
~~~

## Corrections that can close now

### SHIELD-08: todo to done

The controlling independent review is ratified:

- <code>.forge/campaigns/luminumbra-priority-campaign/shield-08-final-review.md:3</code>
  and line 95 state <code>RATIFIED</code>.
- <code>.forge/campaigns/luminumbra-priority-campaign/shield-08-evidence.md:52-58</code>
  records the focused proof, including Far worker 16/16, SHIELD 23/23, and persistence
  61/61.
- The final review states that no scoped correctness, persistence, or proof blocker remains.

Backlog repair must replace the old proposed proving signal with this actual evidence and
record the ratification as a durable closure note.

### OPS-09: closure-ready todo to done with durable digest

The current-HEAD scheduled full gate report is green:

- <code>.forge/artifacts/nightly/2026-07-11-033252.md:6,10-15</code> records PASS for
  Build, UnitTests, EngineFrontierAll, DeterminismMatrixQuick, RenderBudgetBuild, and
  RenderBudget.
- The unit stage reports 1,804 executed tests, zero failures, one disabled test, and one
  skipped test.
- The determinism matrix records debug <code>a66ab4d049ba9228</code> and release
  <code>045f7c2f0645bcce</code>.
- The selected median GPU pass sum is 2.683 ms.
- The report was produced at the reviewed HEAD.

Important caveat: <code>.forge/artifacts/</code> is ignored by
<code>.gitignore:44</code>. The ignored report is valid local evidence but is not a durable
tracked closure by itself. Refresh the tracked OPS-09 review/digest with HEAD, timestamp,
stage results, test counts, baseline hashes, and the artifact path.

## Items that are partially delivered, not done

The following table gives the minimum truth-preserving rewrite. A narrower claim may remain
done only when the old overbroad wording is explicitly replaced; otherwise use the strict
in-progress correction.

| ID | Verified reality | Key evidence | What remains for full closure |
| --- | --- | --- | --- |
| <code>AETHER-07</code> | Generic field emitter/gather infrastructure and an isolated Lua sampler exist. Authored Aether content is not executable in normal play. | <code>src/luminumbra_common/components/FieldEmitterComponents.h:35-43</code>; <code>src/luminumbra_common/scripting/LuaState.cpp:28-40</code>; <code>test/scripting/aether_script_binding_test.cpp:121-154</code>; <code>scripts/common/systems/system_aetheric_feedback.lua:9-22</code>; <code>scripts/common/archetypes/glimmercap.json:13-16</code>. | Add a production-owned Lua host and deterministic lifecycle; reconcile the authored component/API/data shapes; prove a shipped script affects a live participant. |
| <code>AETHER-08</code> | Dual-channel simulation/storage and renderer upload API exist, but <code>update_aether_field_dual()</code> has no production caller. The existing “game content maps” claim is therefore dormant. | <code>src/luminumbra_client/rendering/RenderPipeline.h:426-430</code>; implementation at <code>src/luminumbra_client/rendering/RenderPipeline.cpp:3705</code>; repository call search finds no invocation. | Wire a real active dual-channel source through normal play and prove both polarities affect live content, render output, save/replay, and neutral-off parity. |
| <code>AETHER-10</code> | Glow color/intensity setters and RenderContext propagation exist, but no production tuning/data source calls <code>set_aether_glow()</code>. | <code>src/luminumbra_client/rendering/RenderPipeline.h:432-435</code>; <code>src/luminumbra_client/rendering/RenderContext.h:124-125</code>; <code>src/luminumbra_client/rendering/passes/LightingPass.cpp:165-166</code>. | Add an owned tuning/content source and a user-reachable visual proof; otherwise narrow the old item to setter/context substrate only. |
| <code>AETHER-11</code> | Shader math and an isolated monotonic test exist, but production modulation remains at the default 0.0 and the setter has no caller. | <code>src/luminumbra_client/rendering/RenderPipeline.h:436-442,1375</code>; <code>res/shaders/lighting_pass.frag:660-665</code>; <code>test/rendering/render_smoke_test.cpp:2327-2359</code>; default-off upload at <code>src/luminumbra_client/main_client.cpp:6902-6915</code>. | Wire a real production tuning/data source, activate it deliberately, and prove visible response in the normal renderer. |
| <code>AETHER-12</code> | AI/photo scoring seams and monotonic tests exist, but AI uses one spawn-point sample for the whole registry and production never assigns the photo shot's Aether value. | <code>src/luminumbra_common/world/GameSession.cpp:248-268</code>; only tests assign photo Aether at <code>test/sim/photo_scoring_test.cpp:230-260</code>; production source search finds no shot assignment. | Sample per relevant entity/position and populate real photo captures; prove deterministic behavior and visible scoring without double-counting future AETHER-15 work. |
| <code>ATMO-12</code> | Weather schedule/config seams exist. Plant moisture and fire dryness do not consume the current weather event. | <code>src/luminumbra_common/world/GameSession.cpp:161-171</code>; <code>src/luminumbra_common/world/GameSession.h:226-232</code>; <code>src/luminumbra_client/main_client.cpp:3141-3143</code>; <code>test/sim/weather_event_test.cpp:26-175</code>. | Add production consumers and an end-to-end weather-to-moisture/dryness proof. |
| <code>GPU-P01</code> | The readiness check is source/CMake presence lint. | <code>tools/gates/validate-engine-frontier.ps1:8172-8179</code>. | Require runtime execution, production pass coverage, and a successful production render rather than symbol presence. |
| <code>GPU-P02</code> | A headless Diligent wrapper and flag parser exist in tests. No shipping device/swapchain or production backend selection exists. | <code>src/luminumbra_client/rendering/rhi/Device.cpp:53-64</code>; <code>test/rendering/rhi_device_bringup_test.cpp:73-103</code>; <code>test/CMakeLists.txt:681-686</code>. | Link and select the backend in the shipping client, create the real presentation path, and prove a normal game frame. |
| <code>GPU-P04</code> | Two HLSL files are simplified reflection/sampler surrogates, not semantic production-pass ports. The live compiler path is Slang, not the stale DXC wording. | <code>res/shaders/pilot/debug_view.frag.hlsl:1-8</code>; <code>res/shaders/pilot/ssao.frag.hlsl:1-6</code>; <code>test/rendering/pilot_shader_reflection_test.cpp:144-172</code>; <code>test/CMakeLists.txt:593-621</code>. | Port real pass semantics, execute them through the intended backend, and compare production outputs. |
| <code>GPU-03</code> / <code>GPU-P05</code> | The pilot renders a calibration cube with basic shaders, not DebugView, SSAO, or LightingPass through the production RenderContext. | <code>test/rendering/rhi_pilot_flip_test.cpp:14-17,210-213,238-336</code>. | Make <code>GPU-P05</code> the canonical production-pass pilot and prove a real pass end to end. |
| <code>GPU-09</code> | FLIP metric/calibration infrastructure exists. The claimed second-backend per-pass parity case is skipped. | <code>test/rendering/dual_backend_flip_test.cpp:472-486</code>. | Either narrow the completed claim to FLIP infrastructure or land non-skipped production-pass parity. |
| <code>GPU-P09</code> / <code>GPU-07</code> | Scaled allocations and a final bilinear blit exist. The acceptance proof explicitly disables TAAU. | <code>src/luminumbra_client/rendering/RenderPipeline.cpp:952-955,4278-4313</code>. | Implement and prove the real reduced-scale TAAU resolve/history path without disabling it. |
| <code>GPU-14</code> | PIX seam/module checks may be retained. Nsight behavior is represented by injected doubles; there is no real Nsight capture artifact. | <code>src/luminumbra_client/rendering/CaptureHooks.h:113-117</code>; <code>test/rendering/capture_sdk_trigger_test.cpp:122-150</code>. | Split PIX and Nsight claims or produce a real Nsight-trigger/capture proof. |
| <code>NET-06</code> | Delta encoding and isolated scale tests exist. Production never enables it; the real soak records delta compression false. | <code>src/luminumbra_common/net/ReplicationEndpoint.h:90</code>; <code>src/luminumbra_server/main_server.cpp:2723-2724</code>; <code>test/net/net_soak_over_the_wire_test.cpp:338-343</code>. | Enable it in the actual server/session path and prove loss/jitter/reorder behavior in an over-the-wire soak. |
| <code>NET-11</code> | TCP single-port fan-out exists. GNS still owns one listen and one connection, and the declared GNS multi-connect proof is absent. | <code>src/luminumbra_common/net/LockstepSession.h:294-321</code>; <code>src/luminumbra_common/net/GnsTransport.h:59-60</code>; <code>src/luminumbra_common/net/GnsTransport.cpp:91-95</code>. | Split TCP as done; retain GNS multiaccept under <code>NET-08</code>. Remove the invalid done-to-todo dependency. |

These 17 records divide into eight scaffold/test-only claims and nine dormant or incomplete
production seams. None satisfies implementation, production wiring, reachability, and
current end-to-end proof for its full existing wording.

## Todo items that are already partially implemented

### UI-09: todo to in progress

Resolution, SFX, and music controls remain hidden in
<code>data/ui/settings.rml:12-36</code>; window mode is visible at lines 74-82. SFX and
music callbacks now apply live, so the old claim that all four settings are persist-only is
stale. Resolution/window changes still require a safe application/recreation path. Split:

- Visible, keyboard/controller-reachable controls with correct current values.
- Live audio application and persistence.
- Safe deferred resolution/window recreation with rollback and restart behavior.

Proof must assert DOM reachability and an observable audio/window effect, not only bridge
values.

### OPS-16: todo to in progress

Most listed hard-coded harness debt has been retired. The current debt gate identifies
<code>ServerWorldRunner</code> and <code>main_client</code>; the server still contains
hard-coded spawn/config behavior at
<code>src/luminumbra_server/ServerWorldRunner.cpp:167</code>. Split client content
externalization from hash-visible server spawn/profile externalization. Complete the server
half before expanding authoritative creature behavior.

## Done claims that need truth-preserving wording repairs

- <code>GPU-P03</code> can remain done only as an inert vocabulary/no-re-export scaffold.
  Its backing slot is explicitly unused/null; it is not a functional backend.
- <code>NET-07</code> can remain done only after replacing the nonexistent
  <code>-Mode NetSoak</code> proof with the registered manual NetSoak ctest at
  <code>test/CMakeLists.txt:1501-1518</code> and its artifact assertions.
- <code>INSTINCT-09</code> should say that the canonical substrate is defaulted on and the
  duplicate active path was retired; an inline oracle remains.
- <code>GPU-08</code> remains todo, but its premise is stale: two HLSL files and Slang
  tooling now exist. The remaining work is semantic pass migration and a shipping path.
- <code>RENDER-14</code> should not imply architecture closure without acknowledging that
  <code>src/luminumbra_client/rendering/RenderPipeline.cpp</code> is still roughly 6,100
  lines and <code>src/luminumbra_client/main_client.cpp</code> roughly 10,500 lines at this
  snapshot.

## The 26 existing records that remain todo after strict correction

<code>UI-06</code>, <code>UI-08</code>, <code>UI-10</code>, <code>UI-11</code>,
<code>GPU-08</code>, <code>GPU-P06</code>, <code>FOLIAGE-09</code>,
<code>GPU-P07</code>, <code>GPU-11</code>, <code>GPU-P10</code>,
<code>GPU-13</code>, <code>GPU-P11</code>, <code>GPU-P08</code>,
<code>GPU-P12</code>, <code>GPU-10</code>, <code>GPU-P13</code>,
<code>GPU-P14</code>, <code>NET-08</code>, <code>NET-13</code>,
<code>NET-12</code>, <code>FOLIAGE-07</code>, <code>FOLIAGE-10</code>,
<code>INSTINCT-14</code>, <code>WATER-15</code>, <code>ATMO-13</code>, and
<code>AUDIO-14</code>.

Their important scope corrections are:

- <code>UI-06</code>: authored fake world cards and a hidden load control are not a load
  journey. Split real enumeration/validation, actual session swap/load, and per-save
  thumbnail capture.
- <code>UI-08</code>: leave until every retained UI item is stable, including
  <code>UI-10</code>, <code>UI-11</code>, proposed <code>UI-14</code>,
  <code>UI-15</code>, <code>UI-16</code>, and <code>UI-17</code>; then validate native
  3840x1600 rather than only the current 800x600 three-view smoke fixture.
- <code>UI-10</code>: explicitly depends on <code>UI-14</code> persistent photo/codex
  ownership and a best-capture ID before a real thumbnail detail UI can close.
- <code>UI-11</code>: explicitly depends on <code>UI-14</code> persistent onboarding state;
  a static hint is not a tutorial sequence.
- <code>FOLIAGE-07</code>: split correctness and scale. The current rebake signature omits
  genome, transform, sun, and season even though output consumes them; fix invalidation
  before caching/instancing and the 10k-plant budget.
- <code>FOLIAGE-09</code>: remove mandatory
  <code>GL_ARB_gpu_shader_int64</code> before the relevant compute shader/backend batch.
  Closure requires successful compilation without shader-int64 and GPU/CPU placement-hash
  parity; deleting the extension line alone is insufficient.
- <code>FOLIAGE-10</code>: define a human-approved morphology target before implementation;
  the current generator already has several features named by the broad item.
- <code>NET-08</code>: split dependency pin/build, N-connection listener, then 32-client
  loss/jitter/reorder soak. Do not fetch an unpinned master branch.
- <code>NET-13</code>: prove POSIX TCP through Docker/Linux.
- <code>NET-12</code>: remains hardware/account-dependent and outside the active queue.
- <code>ATMO-13</code>: define a deterministic authoritative anchor set or region-paged
  field windows across weather, wind, Aether, irrigation, soil, scent, save/replay, and
  replication. Single-player may use one anchor, but multiplayer must support widely
  separated players without reset/teleport artifacts. Prove continuity, dispersed-player
  coverage, save/replay identity, and positive movement of weather/fields with their
  owning regions.
- <code>INSTINCT-14</code>: defer until networking, the moving-anchor contract, and the
  server half of OPS-16 are stable.
- <code>WATER-15</code>: split render-only channel detail from deterministic
  seam-resampled simulation; they have different hash and proof obligations.
- <code>AUDIO-14</code>: wait for final assets, then require bank integrity and audible
  review, not literal/event presence alone.

## Candidate ID and canonical-pillar treatment

Spec 021 currently permits only these 12 pillars:
<code>aetheric</code>, <code>atmospheric</code>, <code>audio</code>,
<code>buildtestops</code>, <code>foliage</code>, <code>gpu</code>,
<code>instinct</code>, <code>networking</code>, <code>render</code>,
<code>shield</code>, <code>ui</code>, and <code>water</code>. See
<code>docs/audit/021/validate_backlog.py:17-29</code> and the taxonomy requirement in
<code>docs/specs/021-engine-framework-audit-charter/spec.md:77,116-128</code>.

Do not add new pillar values such as persistence, security, assets, or gameplay directly to
the JSON unless the charter and validator are deliberately revised. Fold the domain into a
canonical pillar and preserve the more specific domain as prose or a new
<code>domain_label</code> field.

The mappings below are compatibility mappings, not a claim that UI semantically owns all
input, SHIELD all persistence/gameplay, or buildtestops all asset/security concerns. Before
canonical integration, the owner may instead revise the charter, validator, pillar
documents, and ranking atomically to introduce durable gameplay, persistence,
input/platform, asset, and security domains. Do not expand the enum silently or create a
parallel backlog.

The review used descriptive shorthand before candidate sequence numbers were checked. Use
the following canonical candidates during integration:

| Review shorthand | Canonical candidate | Pillar | Domain label |
| --- | --- | --- | --- |
| <code>INPUT-01</code> | <code>UI-13</code> | ui | player/input |
| <code>PERSIST-01</code> | <code>SHIELD-18A</code> + <code>SHIELD-18B</code> | shield | persistence transaction + completeness audit |
| <code>PHOTO-01</code> | <code>UI-14</code> | ui | profile/world photography persistence |
| <code>DET-01</code> | <code>OPS-17</code> | buildtestops | determinism contract |
| <code>VIS-01</code> | <code>RENDER-21</code> | render | visual-quality gate |
| <code>DOC-TRUTH-01</code> | <code>OPS-18</code> | buildtestops | capability truth |
| <code>PROFILE-01</code> | <code>OPS-19</code> | buildtestops | integration/profile |
| <code>ASSET-01</code> | <code>OPS-20</code> | buildtestops | asset reachability |
| <code>SCRIPT-01</code> | <code>AETHER-13</code> | aetheric | production Lua host |
| <code>SCRIPT-02</code> | <code>AETHER-14</code> | aetheric | Lua sandbox/API parity |
| <code>WORLD-01</code> | <code>INSTINCT-16</code> | instinct | authoritative wildlife |
| <code>CREATURE-VIS-01</code> | <code>INSTINCT-17</code> | instinct | species presentation |
| <code>NET-UX-01</code> | <code>NET-14</code> | networking | player host/join |
| <code>PERF-01</code> | <code>OPS-21</code> | buildtestops | playable performance floor |
| <code>UX-A11Y-01</code> | <code>UI-15</code> | ui | accessibility/localization/controller |
| <code>OPS-PACKAGE-01</code> | <code>OPS-22</code> | buildtestops | packaging |
| <code>SECURITY-01</code> | <code>OPS-23</code> | buildtestops | hostile-input hardening |
| <code>PARTICLE-01</code> | <code>RENDER-23</code> | render | particles/spec 007 |
| <code>SPEC-008-TRACE</code> | <code>OPS-24</code> | buildtestops | spec reconciliation |
| <code>PHOTO-DEPTH-01</code> | <code>UI-16</code> | ui | photography/spec 012 |
| <code>POI-01</code> | <code>SHIELD-19</code> | shield | POI phase 2 |
| <code>POI-02</code> | <code>SHIELD-20</code> | shield | POI phase 3 |
| Aether activation | <code>AETHER-15</code> | aetheric | real participants/activation |
| <code>SHADER-AUTHOR-02</code> | <code>RENDER-24</code> | render | shader/pass authoring |
| <code>CELESTIAL-02</code> | <code>ATMO-16</code> | atmospheric | multi-body render tier |
| <code>GAMEPLAY-ITEMS-01</code> | <code>SHIELD-21</code> | shield | inventory/craft/place |
| <code>AUTHORING-01</code> | <code>SHIELD-22</code> | shield | World Painter/.lworld |
| <code>PERF-CACHE-01</code> | <code>SHIELD-23</code> | shield | bounded worldgen cache |
| <code>ARCH-CLIENT-01</code> | <code>OPS-25</code> | buildtestops | lifecycle architecture |
| <code>AUDIO-15</code> | <code>AUDIO-15</code> | audio | asset/processing completion |
| <code>UI-WORLDLIST-02</code> | <code>UI-17</code> | ui | world-library behavior |
| Palette/material calibration | <code>RENDER-22</code> | render | visual calibration |
| Tree visual polish | <code>FOLIAGE-12</code> | foliage | morphology/intersection/LOD |
| Water visual coherence | <code>WATER-18</code> | water | water/shore/weather quality |
| Lifecycle soak | <code>OPS-26</code> | buildtestops | long-running lifecycle health |

Recheck ID availability immediately before editing the canonical JSON. The table expresses
the recommended sequence and taxonomy; it does not reserve IDs by itself.

### Epic decomposition required before dispatch

Several candidates describe program outcomes, not implementation-sized tasks. Keep the
parent for traceability, but create individually estimated children with their own
dependencies and proving signals:

- <code>UI-15</code>: controller input/rewiring; focus/navigation; UI/text scaling;
  contrast/color-safe/reduced-motion/captions; localization extraction and pseudo-locale.
- <code>OPS-20</code>: typed schema/owner inventory; build-time reachability graph;
  orphan/cycle/duplicate-owner failures; packaging-manifest integration.
- <code>OPS-23</code>: canonical save-path containment; malformed/oversized save corpus;
  network parser bounds/fuzzing; Lua resource limits/escape corpus; sanitizer reporting.
- <code>INSTINCT-17</code>: species asset descriptors; distinct rigs/materials; behavior
  animation graph; LOD/motion budgets; unlabeled human-approved capture grid.
- <code>AETHER-15</code>: active emitter/sink participants; dual-channel upload and tuning;
  per-entity AI/photo consumption; intended-profile activation; persistence,
  replication/replay, and neutral-off parity.

Do not dispatch an epic as one undifferentiated coding assignment.

### Candidate dependency spine

This is dependency guidance, not a replacement numeric ranking:

| Candidate | Priority | Hard/closure dependencies |
| --- | --- | --- |
| <code>OPS-17</code> | P0 decision where blocking | None; settle before persistence/network compatibility is declared complete, but do not block unrelated feature coding. |
| <code>UI-13</code> | P0 | None. |
| <code>SHIELD-18A</code> | P0 | <code>OPS-17</code> contract decision and <code>OPS-19</code> intended-profile definition for closure. |
| <code>SHIELD-18B</code> | P1 final integration | All retained authoritative payload owners, including <code>UI-14</code>, ATMO/Aether/Instinct/network state. |
| <code>UI-14</code> | P0 | <code>SHIELD-18A</code>; input ownership from <code>UI-13</code> for the capture journey. |
| <code>RENDER-21</code> | High parallel lane | None; must precede subjective visual closure, but does not block save/load/input work. |
| <code>OPS-18</code> | Phase-0 governance | None; should precede recommitting aspirational features. |
| <code>OPS-19</code> | Phase-0 foundation | <code>OPS-18</code> capability decision; define before snapshot completeness or feature activation is claimed. |
| <code>OPS-20</code> | P1 | None; required before broad script/content activation and packaging. |
| <code>AETHER-13</code> | P1 | <code>OPS-17</code>, <code>SHIELD-18A</code>, <code>OPS-20</code>. |
| <code>AETHER-14</code> | P1 | <code>AETHER-13</code>. |
| <code>AETHER-15</code> | P1 | <code>AETHER-13</code>, <code>AETHER-14</code>, <code>ATMO-13</code>, <code>OPS-19</code>, <code>OPS-20</code>, <code>SHIELD-18A</code>. |
| <code>INSTINCT-16</code> | P1 | <code>OPS-17</code>, <code>SHIELD-18A</code>, <code>OPS-19</code>. |
| <code>INSTINCT-17</code> | P1 | <code>INSTINCT-16</code>, <code>OPS-20</code>, <code>RENDER-21</code>. |
| <code>NET-14</code> | P1 | <code>UI-13</code>, <code>SHIELD-18A</code>, <code>OPS-17</code>, and the TCP-complete portion of <code>NET-11</code>; it need not wait for GNS scale-out. |
| <code>OPS-21</code> | P1 | <code>OPS-19</code> and a representative shipping profile/world. |
| <code>UI-15</code> | P1 | <code>UI-13</code> and stabilized UI journeys. |
| <code>OPS-22</code> | P1 | <code>OPS-19</code>, <code>OPS-20</code>, real save/load smoke. |
| <code>OPS-23</code> | P1 | Acceptance spans <code>SHIELD-18A</code>, <code>AETHER-13/14</code>, and network parsers. |
| <code>RENDER-22</code> | P1 | <code>RENDER-21</code>. |
| <code>FOLIAGE-12</code> | P1/P2 | FOLIAGE-07 correctness plus <code>RENDER-21</code>; merge with FOLIAGE-10 if scopes overlap. |
| <code>WATER-18</code> | P1/P2 | <code>RENDER-21</code>; coordinate with WATER-15A. |
| <code>OPS-26</code> | P1/P2 | Real save/load, photo, networking, streaming, and teardown journeys. |
| <code>RENDER-23</code> | P2 | Motion-vector/TAAU contract and budget target. |
| <code>OPS-24</code> | P2 reconciliation | None; perform before creating duplicate spec-008 tasks. |
| <code>UI-16</code> | P2 | <code>UI-13</code>, <code>UI-14</code>. |
| <code>SHIELD-19</code> | P2 | <code>OPS-20</code>, <code>SHIELD-18A</code>. |
| <code>SHIELD-20</code> | P2 | <code>SHIELD-19</code>. |
| <code>RENDER-24</code> | P2 | Stable render-resource/pass contracts and package ownership. |
| <code>ATMO-16</code> | P2 | Authoritative simulation time and <code>RENDER-21</code>. |
| <code>SHIELD-21</code> | Product decision | <code>AETHER-13/14</code> for Lua recipes, <code>SHIELD-18A</code>, UI, and networking. |
| <code>SHIELD-22</code> | Product decision | <code>OPS-20</code>, <code>SHIELD-18A</code>. |
| <code>SHIELD-23</code> | P2 | Representative world/performance workloads and bit-exact parity gate. |
| <code>OPS-25</code> | Architectural | Preserve proven journeys first; refactor with lifecycle/teardown parity. |
| <code>AUDIO-15</code> | Asset-dependent | Final approved assets and audible review target. |
| <code>UI-17</code> | P1 | <code>SHIELD-18A</code> and real save metadata. |

## Missing safety-critical and Phase-0 backlog coverage

These records should be added before the old ranking is dispatched. The true runtime P0 is
UI-13, SHIELD-18A, UI-14, and the OPS-17 decision where it blocks save/protocol
compatibility. RENDER-21 is a high-priority parallel visual lane, and OPS-18 is Phase-0
governance. IDs are proposed and should be checked for uniqueness during the canonical
backlog edit.

### UI-13 (review shorthand INPUT-01): context-aware input routing

Problem:

- <code>V</code> is ToggleNoclip in
  <code>src/luminumbra_client/player/InputActions.h:15-48,71</code> and is also polled
  directly to cycle farm species at
  <code>src/luminumbra_client/main_client.cpp:7351-7363</code>.
- <code>T</code> is PhotoWeatherCycle in InputActions and is also polled directly for
  terrain fill at <code>src/luminumbra_client/main_client.cpp:7428-7483</code>.
- Independent raw GLFW handlers can fire outside the intended context.

Required proof: contextual actions change only the intended state; photo-mode T changes
weather without changing chunk hashes; rebinding coverage is complete; a gate rejects raw
gameplay GLFW polling outside the input owner.

### SHIELD-18A/18B (review shorthand PERSIST-01): transactional persistence and final completeness

Problem:

- <code>GameSession::SaveWorldState</code> saves chunks, plants, and the energy field at
  <code>src/luminumbra_common/world/GameSession.cpp:1033-1144</code>.
- Restore covers the same limited set at lines 1147-1265.
- Metadata omits simulation tick, creatures, player state, weather, scent, progression, and
  other authoritative runtime state at lines 990-1021.
- Shutdown calls <code>SaveWorldState()</code>, not the metadata-writing
  <code>SaveWorld()</code> path, at
  <code>src/luminumbra_client/main_client.cpp:10565</code> in the audited snapshot.

Do not make one early item claim “complete snapshots” before later authoritative schemas
exist. Split ownership:

- <code>SHIELD-18A</code> owns the versioned transaction/manifest, atomic multi-payload
  commit, payload registration, migration, corruption behavior, interrupted-write rollback,
  and current-state save/load framework. <code>OPS-19</code> defines which systems are in
  the intended profile. Feature items own serialization of their payloads.
- <code>SHIELD-18B</code> is the final completeness audit after retained ATMO, Aether,
  Instinct, networking, player/progression, and photo payloads exist. It verifies that the
  intended profile has no unregistered authoritative state.

Required proof for 18A: atomic replacement and clean rollback across registered payloads,
version migration, corruption quarantine, interrupted-write recovery, and continuous T+N
versus save/load T+N for the currently registered state.

Required proof for 18B: repeat the T+N comparison across every authoritative subhash and
player/progression payload in the final intended profile. <code>UI-06</code> depends on
18A's trustworthy transaction/load path, not on every future payload required by 18B.

### UI-14 (review shorthand PHOTO-01): profile/world photo library and codex persistence

Problem:

- Photo state, codex, objectives, and capture counter are process globals around
  <code>src/luminumbra_client/main_client.cpp:120-136</code>.
- Capture IDs derive from an in-memory counter around line 7807.
- Output uses global <code>photos/photo-N.*</code> and
  <code>data/ui/captures/cap_N.tga</code> around lines 7854-7888.
- Restart can overwrite captures; worlds can share/mix them; codex/objective progress is
  not durable.
- The shutter path synchronously calls <code>glReadPixels</code> around line 7866, so
  capture latency also needs an explicit budget.

Ownership decision for this handoff: photo captures, codex progress, objectives, scores,
behavior masks, and best-capture associations are world-scoped beneath an explicit
profile/world user-data partition. Profile-global settings remain separate. UI-14 owns
these payload schemas and registers them with SHIELD-18A; SHIELD-18A owns the transaction
boundary and recovery rules.

Required proof: two clean-process runs preserve all prior captures and world-scoped
progression; IDs are monotonic; profile/world switching cannot overwrite or cross-link
files; sidecars and thumbnail associations are referentially valid and reconstructible;
capture latency remains within a named budget.

### OPS-17 (review shorthand DET-01): resolve the determinism contract

Problem:

- <code>docs/specs/018-determinism-hardening/spec.md:181-182</code> requires
  identical Debug and Release simulation hashes.
- <code>tools/gates/validate-determinism-matrix.ps1:36-40,185-186</code> permits
  per-build baselines.
- <code>README.md:28</code> makes a narrower same-seed/identical-worlds-across-platforms
  claim; it does not by itself prove full simulation or Debug/Release hash identity.

Decision required: either implement identical hashes across supported builds/platforms, or
amend the spec, README, protocol compatibility rules, and tests to the narrower guarantee.
Do not silently keep contradictory contracts.

### RENDER-21 (review shorthand VIS-01): a visual-fidelity gate that rejects known-bad output

Problem: the objective critique reports zero defects for a sweep whose montages contain
obvious visible defects. See the dedicated visual-fidelity section below.

Required proof: known-bad captures fail by flaw taxonomy; comparisons cover biomes, seeds,
weather, and time of day; reference targets and human/art-direction approval complement
metrics; no automatic rebless can turn new output green.

### OPS-18 (review shorthand DOC-TRUTH-01): capability-to-proof matrix

Problem: README claims around <code>README.md:102,156-165,176-185,220,225</code> exceed
demonstrated product behavior, including live Lua archetypes/hot reload, World Painter and
<code>.lworld</code>, collection/crafting/placement with Lua recipes, Steam networking, and
the scope of same-seed cross-platform world identity.

Required proof: every “available” capability links to a packaged-game journey and current
gate. Unsupported capabilities are labeled prototype/planned or removed.

## Missing foundation and production-wiring coverage

| Proposed ID | Required outcome | Evidence for the gap |
| --- | --- | --- |
| <code>OPS-19</code> | Define the complete intended shipping systems profile, make every schema key explicit, and run full journeys against it. | <code>data/common/systems.game.json:1-10</code> calls itself shipping-on but has four false entries; <code>data/common/systems.json:3-56</code> leaves major systems disabled while some consumers remain enabled. |
| <code>OPS-20</code> | Build a typed content reachability/ownership graph. Every shipped asset needs a schema, owner, and reachable consumer or an explicit fixture/reference/deferred classification. | <code>tools/check_runtime_data_manifest.py:17-48</code> checks copy-list completeness, not runtime consumption. |
| <code>AETHER-13</code> | Add a production-owned Lua host, approved script loading, stable fixed-tick execution, deterministic command ordering, and a real live participant. | <code>src/luminumbra_common/scripting/LuaState.cpp:20-79</code> exposes only an isolated sampler/evaluator; no production session owns it. <code>scripts/common/ai/actions/action_find_water.lua:7-52</code> is copied from/returns the wrong food action type. |
| <code>AETHER-14</code> | Enforce live sandboxing, instruction/CPU/memory budgets, quarantine/rollback, manifest/API parity, and compile/load validation for the shipped corpus. | <code>src/luminumbra_common/scripting/LuaApiManifest.cpp:76-90</code> advertises ten APIs while <code>src/luminumbra_common/scripting/LuaState.cpp:28-39</code> binds only the energy sampler; <code>test/scripting/lua_sandbox_escape_test.cpp:1-20</code> describes a manifest-only/stub guarantee. |
| <code>INSTINCT-16</code> | Replace client-decoration wildlife with one authoritative, deterministic, data-driven roster used by single player, host, and server; save, replicate, and replay it. | <code>src/luminumbra_client/main_client.cpp:6170-6178,6434-6439</code> describes client decoration; <code>src/luminumbra_server/ServerWorldRunner.h:52-65</code> exposes a different default-off roster. |
| <code>INSTINCT-17</code> | Deliver distinct species rigs, materials, clips, behavior animation graphs, LOD, and visual approval. | <code>src/luminumbra_client/main_client.cpp:6193-6203,6370-6409,6461-6465</code> reuses one grovestrider rig/idle clip with tint/scale variation for ten species. |
| <code>NET-14</code> | Add a player-facing host/join vertical slice using the actual client. This was also described during review as <code>NET-UX-01</code>; choose one canonical ID. | Current client networking is loopback/harness oriented; no normal host/join UI/session path was found. |
| <code>OPS-21</code> | Certify an absolute playable-path floor on named target hardware for idle, pan, streaming, dense forest, ecology, and weather, with medians/p99 and provenance. | Spec 004 was rescoped after its premise failed; <code>tools/gates/baselines/perf-floor-release.json</code> remains unblessed and the frontier script calls the 300 FPS floor uncertified. |
| <code>UI-15</code> | Establish keyboard/controller journeys, focus order, scalable UI/text, contrast/color-safe modes, reduced motion, captions, localization keys, and pseudo-localization. | <code>docs/UI_REFACTORING_SUMMARY.md:192-198</code> lists accessibility/platform optimization as unimplemented; InputActions models keyboard keys only. |
| <code>OPS-22</code> | Produce reproducible client/server packages and a clean-machine smoke with licenses, symbols, hashes, provenance, runtime data, shaders, and DLLs. | <code>.github/workflows/ci.yml:15-63</code> builds/tests, but no substantive install/CPack and clean distributable launch path was found. |
| <code>OPS-23</code> | Harden save paths, malformed/oversized saves and network messages, script resources, parser fuzzing, and approved write roots. Treat this as a hardening audit, not a confirmed exploit. | <code>src/luminumbra_common/world/GameSession.cpp:843-850</code> builds a path from caller-provided world ID; no unified hostile-input proof exists. |

For <code>NET-14</code>, closure means two clean client build executables join one host,
exchange authoritative world/avatar state and input, render movement, handle
disconnect/reconnect and useful error/cancel states, and preserve save authority. The
stronger packaged/clean-machine variant belongs to <code>OPS-22</code>, avoiding a circular
dependency.

Additional cross-cutting quality candidates:

- <code>RENDER-22</code>: palette, material, and lighting calibration across clear/storm
  dawn/noon/dusk/night comparisons, with explicit art approval.
- <code>FOLIAGE-12</code>: expand or supersede existing FOLIAGE-10 rather than duplicate it;
  close camera-intersecting black branches, canopy/trunk silhouette defects, species
  sameness, and unstable LOD transitions.
- <code>WATER-18</code>: water/shore/weather visual coherence, including readable
  depth/absorption/reflection, coherent shore transition, visible channels/waterfalls, and
  temporal storm/lightning review.
- <code>OPS-26</code>: a long-running lifecycle soak covering repeated world
  create/load/quit, photo capture, network reconnect, and streaming, with stable RSS/VRAM,
  job/chunk counts, graphics handles, and no late teardown work.

## Missing specification reconciliation and completion work

| Proposed ID | Required disposition | Evidence |
| --- | --- | --- |
| <code>RENDER-23</code> | Resume spec 007. Its recorded “no motion-vector MRT” blocker is stale because GBuffer attachment 4 is now RG16F. Finish richer descriptors, particle vectors consumed by TAAU, no-ghost proof, CPU/GPU parity, forces/atlas/depth collision, and budgets. | <code>docs/specs/007-particle-system/spec.md:3-11,45-52</code>; <code>src/luminumbra_client/rendering/passes/GBufferPass.cpp:166-193</code>; <code>src/luminumbra_client/rendering/passes/ParticlePass.h:50-59</code>. |
| <code>OPS-24</code> | Map spec 008 WS-1 through WS-4 to current code, live gates, and evidence. The code contains substantial sky/depth/collision/far-LOD work while the acceptance record remains unchecked. | <code>docs/specs/008-gpu-locked-perf-landables/spec.md:42-100</code>; representative GPU sky-LUT use at <code>src/luminumbra_client/main_client.cpp:3232-3234</code>. |
| <code>UI-16</code> | Reconcile spec 012. Exposure controls and metadata landed; the requested histogram/focus feedback and complete photo journey did not. | <code>docs/specs/012-photography-depth/spec.md:139-179,241-268</code>; <code>src/luminumbra_client/main_client.cpp:655-722,7685-7796</code>; <code>src/luminumbra_client/rendering/RenderPipeline.h:607-612</code>. |
| <code>SHIELD-19</code> | Complete spec 013 phase 2: biome-aware POI vocabulary, density/type matrix, and recognizable hero landmarks with locate/approach/photo/save/load proof. | <code>docs/specs/013-points-of-interest/spec.md:49-52,94-107,137-147</code>; <code>src/luminumbra_common/systems/SHIELD_WorldSystem.cpp:4355-4383</code>; <code>src/luminumbra_common/world/StructurePlacement.cpp:124-129</code>. |
| <code>SHIELD-20</code> | Complete spec 013 phase 3: deterministic socket-kit assembly, seams/transitions/focal pieces, materially distinct silhouettes, weathering, collision, LOD, and budget proof. | <code>docs/specs/013-points-of-interest/spec.md:108-113,146-147</code>. |
| <code>AETHER-15</code> | Activate spec 024 through a real emitter, sink, and sensitive creature; connect visual/photo response; persist, replicate, replay, and hash the state while preserving a neutral off path. | <code>docs/specs/024-aether-field-completion/spec.md:3,213</code>; <code>data/common/systems.json:40-41</code>; <code>src/luminumbra_common/world/GameSession.cpp:489-492</code>; only tests instantiate emitters at <code>test/scripting/aether_script_binding_test.cpp:118-147</code>. |
| <code>RENDER-24</code> | Implement spec 023's data-driven material/pass executor, validation, hot-reload rollback, packaging whitelist, and disabled-path parity. | <code>docs/specs/023-live-shader-authoring/spec.md:3,53-55</code>. |
| <code>ATMO-16</code> | Implement spec 022 tier 2: data-driven multiple bodies while retaining a single primary shadow owner and default single-sun/moon parity. | <code>docs/specs/022-celestial-body-seam/spec.md:3,36-38</code>. |

The backlog currently has no explicit <code>spec</code> values for 004, 007, 008, 012, 022,
023, or 024. That absence is one reason the above work disappeared from the active plan.

## Product claims needing explicit keep/remove decisions

The following should become backlog entries if they remain product commitments; otherwise
the public/internal claims should be removed or clearly labeled planned:

- <code>SHIELD-21</code>: collect to inventory to Lua recipe to craft to place,
  including save and replication. The claim appears at <code>README.md:170-185</code>; the
  existing farming counters are not the described generic item/crafting system.
- <code>SHIELD-22</code>: usable World Painter and <code>.lworld</code> import/export,
  currently claimed around <code>README.md:152-165</code> without a usable editor path.
- <code>SHIELD-23</code>: measured, bounded world-generation cache with exact parity,
  eviction, telemetry, and demonstrated net benefit. The design claim is in
  <code>docs/specs/engine-caching-system.md:3,44-64</code>.
- <code>OPS-25</code>: decompose the roughly 10,500-line client main loop and roughly
  6,100-line render pipeline, while also reducing the roughly 4,900-line
  <code>src/luminumbra_common/systems/SHIELD_WorldSystem.cpp</code> and 1,300-line
  <code>src/luminumbra_common/world/GameSession.cpp</code> where lifecycle ownership
  requires it. Produce explicit owners with teardown/world-transition tests; reconcile the
  claim against <code>RENDER-14</code>, which is currently marked done.
- <code>AUDIO-15</code>: replace placeholder audible assets and complete per-source
  environmental processing rather than log-only/TODO behavior. Evidence includes
  <code>data/audio/sfx_main.bank.json:176-189</code>,
  <code>src/luminumbra_client/audio/MiniaudioManager.cpp:816-820</code>, and
  <code>src/luminumbra_client/audio/AudioSpatialCluster.cpp:324-337</code>.
- <code>UI-17</code>: finish real loading, favorites, recent filters, timestamps,
  and creation dates in the world library; current placeholders are visible at
  <code>src/luminumbra_client/ui/components/game/WorldList.cpp:369,392,411,416,436,469</code>.

## Visual-fidelity audit

Artifacts inspected:

- <code>build/release/test-artifacts/runtime/world-visual-sweep/sweep/montages/montage_summer_dawn.png</code>
- <code>build/release/test-artifacts/runtime/world-visual-sweep/sweep/montages/montage_summer_noon.png</code>
- <code>build/release/test-artifacts/runtime/world-visual-sweep/sweep/montages/montage_summer_dusk.png</code>
- <code>build/release/test-artifacts/runtime/world-visual-sweep/sweep/montages/montage_summer_night.png</code>
- <code>build/release/test-artifacts/runtime/world-visual-sweep/objective-critique.md</code>

The captures are suitable known-bad regression fixtures. Their exact capture HEAD, runtime
configuration, seed matrix, and shader/executable provenance must be recorded before anyone
describes them as current-HEAD acceptance evidence.

Visible problems include:

- Large black branches intersecting the camera.
- Disconnected, clipped, or cutout-looking canopy pieces.
- Neon cyan/blue terrain and foliage.
- Flat ultramarine water and hard shoreline bands.
- Uniform extreme-red dusk treatment.
- Overlit white ground at night.
- Coarse cloud, aurora, and lightning treatment.
- Repeated compositions that do not demonstrate biome/seed breadth.

The objective critique reports 0/48 defects. That result is not credible subjective-fidelity
proof. <code>tools/visual_critique.py</code> primarily applies pixel heuristics, which can
detect limited technical anomalies but cannot approve composition, material credibility,
silhouette quality, camera intersections, art direction, or scene variety.

<code>docs/audit/021/stranded/README.md</code> and
<code>docs/audit/021/stranded/grass-overhaul-2026-06.patch</code> preserve an unlanded
visual-tuning diff. They are useful historical context, but must not be treated as current
implementation or fidelity proof.

The replacement gate should:

1. Define a flaw taxonomy with known-bad positive fixtures.
2. Fail the current known-bad montage cells for named reasons.
3. Sample multiple seeds, biomes, weather states, times, camera heights, and near/far
   compositions.
4. Compare against curated reference targets where applicable.
5. Require owner/art-direction approval for reblessing.
6. Preserve prior approved captures and reject automatic baseline replacement.
7. Track per-cell disposition so a numeric average cannot hide a severe defect.

Visual repair should occur on the existing OpenGL production path before broad GPU-backend
migration. Otherwise fidelity defects and backend differences will be confounded.

## Global backlog integrity defects

1. <code>backlog.json:2-8</code> still says generated 2026-07-02 and stores obsolete
   determinism hashes.
2. Current static baselines are debug <code>a66ab4d049ba9228</code> and release
   <code>045f7c2f0645bcce</code>.
3. Twenty-one done records contain the old debug hash
   <code>6f008a9f637c40b7</code>:
   <code>SHIELD-11</code>, <code>ATMO-01</code>, <code>ATMO-02</code>,
   <code>WATER-03</code>, <code>FOLIAGE-04</code>, <code>AETHER-02</code>,
   <code>SHIELD-16</code>, <code>SHIELD-02</code>, <code>SHIELD-17</code>,
   <code>SHIELD-01</code>, <code>OPS-13</code>, <code>RENDER-06</code>,
   <code>SHIELD-06</code>, <code>GPU-P02</code>, <code>GPU-P05</code>,
   <code>RENDER-09</code>, <code>RENDER-14</code>, <code>INSTINCT-07</code>,
   <code>ATMO-11</code>, <code>INSTINCT-10</code>, and <code>ATMO-10</code>. Replace it
   only where it is presented as the current baseline; preserve explicitly labeled
   historical evidence.
4. Seventy-two done proving signals still contain <code>NEW:</code>, which describes a
   proposed gate rather than durable closure evidence.
5. None of the 190 records has a structured <code>closure_note</code> property.
6. <code>priority-ranking.md:3</code> says 183 items although the backlog contains 190.
7. Its tail still says 160/190 done and names OPS-09/SHIELD-08 as open work.
8. <code>pillar-networking.md:226-233</code> disagrees with the canonical JSON on several
   NET statuses.
9. <code>NET-11</code> is the only done record depending on a todo record,
   <code>NET-08</code>.
10. The validator checks schema, enums, nonempty strings, and dependency syntax. It does not
    resolve evidence paths, validate test/mode existence, reject skipped/disabled proof,
    detect production-call gaps, identify duplicates, or reject unexplained done-to-open
    dependencies.

## Duplicate, merge, and supersede map

Apply this before assigning new ranks:

| Historical/umbrella record | Canonical disposition |
| --- | --- |
| <code>GPU-03</code> | Merge into <code>GPU-P05</code>; reopen the canonical item as a real production-pass pilot. |
| <code>GPU-08</code> | Superseded by <code>GPU-P06</code>. |
| <code>GPU-11</code> | Superseded by <code>GPU-P10</code>. |
| <code>GPU-13</code> | Superseded by <code>GPU-P11</code>. |
| <code>GPU-10</code> | Split/superseded by <code>GPU-P12</code> and <code>GPU-P13</code>. |
| <code>GPU-07</code> | Historical parent of <code>GPU-P09</code>; do not count both as independent completed work. |
| <code>GPU-P14</code> | Keep separate. |
| <code>AETHER-07</code> | Keep in progress with <code>AETHER-13</code> as its production-host completion parent, or narrow AETHER-07 to substrate-only done and move all delivery scope to AETHER-13. |
| <code>AETHER-08</code>, <code>AETHER-10</code>, <code>AETHER-11</code>, <code>AETHER-12</code> | Link their dormant production scope to <code>AETHER-15</code> children, or narrow each old record explicitly before granting substrate-only completion. Do not credit both layers for the same delivery. |
| <code>SHIELD-08</code>, <code>OPS-09</code> | Close as done; do not mark superseded. |

Use an explicit <code>superseded_by</code> or <code>canonical_id</code> field so historical
records remain traceable without inflating active or completed counts.

## Recommended execution program

### Phase 0: repair planning truth

Before dispatching the old numeric ranking:

1. Refresh backlog metadata and evidence provenance.
2. Apply the reopen/todo-to-in-progress corrections and close ratified SHIELD-08, yielding
   the 144 done / 27 todo / 19 in-progress interim ledger.
3. Close OPS-09 only in the same change that adds its tracked durable digest, yielding the
   145 done / 26 todo / 19 in-progress target.
4. Narrow conditional done claims and split NET-11 TCP/GNS scope.
5. Apply the GPU merge/supersede map.
6. Perform <code>OPS-18</code> capability-truth decisions and define the intended shipping
   profile under <code>OPS-19</code>.
7. Add and, where necessary, decompose the missing P0/P1/spec-reconciliation candidates.
8. Generate a fresh active queue separately from historical ranking.
9. Strengthen validation and then update all pillar/ranking counts atomically.

### Phase 1: protect player state and planning contracts

The true safety-critical P0 is narrower than the full candidate set:

1. <code>OPS-17</code>: settle the determinism/compatibility decision where it blocks
   save formats and network protocol claims. It need not block unrelated work.
2. <code>UI-13</code>: remove destructive context conflicts.
3. With <code>OPS-19</code> defining intended state, complete <code>SHIELD-18A</code>
   transaction/manifest/registration infrastructure.
4. <code>UI-14</code>: register profile/world photo, codex, objective, and capture payloads
   with SHIELD-18A and eliminate overwrite/cross-world hazards.
5. In a parallel high-priority lane, <code>RENDER-21</code> creates a gate capable of
   rejecting current known-bad visuals.

Persistence and the real world-load UI can be developed in parallel, but <code>UI-06</code>
depends on SHIELD-18A's trustworthy transaction/load path, not the late SHIELD-18B
all-system completeness audit.

### Phase 2: restore core product journeys

1. Split <code>UI-06</code> into real save enumeration/validation, actual session load/world
   swap, and per-save thumbnails.
2. Complete <code>UI-09</code> with visible controls and observable effects.
3. Finish <code>UI-14</code>, then <code>UI-10</code> and <code>UI-11</code>.
4. Complete <code>UI-17</code> if the world-library product commitment remains.
5. Defer final <code>UI-08</code> approval until retained <code>UI-15</code>,
   <code>UI-16</code>, and <code>UI-17</code> are also stable.

### Phase 3: correctness and authoritative simulation

1. Split <code>FOLIAGE-07A</code> correctness from <code>FOLIAGE-07B</code> scale/cache;
   07B depends on 07A.
2. Close reopened <code>ATMO-12</code> by connecting weather events to real plant-moisture
   and fire-dryness consumers.
3. Define and migrate the deterministic anchor set/region-paged field-window contract under
   <code>ATMO-13</code>.
4. Complete <code>OPS-20</code> asset reachability before activating scripts, Aether
   content, distinct species assets, or packages.
5. Add the actual client host/join slice as <code>NET-14</code>, using the completed TCP
   portion of NET-11. Its proof uses two clean client build executables; packaged
   clean-machine proof remains owned by OPS-22.
6. Enable and prove production delta compression under reopened <code>NET-06</code> on the
   TCP baseline, then complete GNS <code>NET-08</code> and POSIX <code>NET-13</code>.
7. Relate reopened AETHER-07 to <code>AETHER-13</code> production Lua ownership, and
   AETHER-08/10/11/12 to <code>AETHER-15</code> activation, using explicit
   completion-parent/child links so substrate and delivery are not credited twice.
8. Land <code>AETHER-13</code> production Lua ownership and <code>AETHER-14</code> live
   sandbox/API parity, then complete <code>AETHER-15</code> activation in explicit slices:
   - real emitters/absorbers,
   - a production caller for dual-field update,
   - production tuning for glow/material modulation,
   - per-entity AI sampling,
   - photo-shot Aether population,
   - save/replication/replay activation.
9. Complete the server half of <code>OPS-16</code>. With NET-14/08 and ATMO-13 stable,
   establish the authoritative roster under <code>INSTINCT-16</code>.
10. Only after INSTINCT-16, add authoritative homes/nests under
    <code>INSTINCT-14</code>.

### Phase 4: quality, content, and packaging

1. Use <code>RENDER-21</code> to drive <code>RENDER-22</code> palette/material/lighting
   calibration through pinned human-reviewed comparisons.
2. <code>INSTINCT-17</code> and human-approved species readability.
3. Reconcile <code>FOLIAGE-10</code> with <code>FOLIAGE-12</code> into one measurable
   morphology, intersection, and LOD target.
4. <code>WATER-15A</code> render detail and <code>WATER-18</code> visual coherence; add
   <code>WATER-15B</code> deterministic simulation only if visually necessary.
5. <code>AUDIO-14</code>/<code>AUDIO-15</code> when final assets exist.
6. Complete retained <code>UI-15</code>, <code>UI-16</code>, and
   <code>UI-17</code>, then run <code>UI-08</code> last with owner-reviewed native
   ultrawide captures.
7. Complete <code>OPS-23</code> hostile-input hardening and <code>OPS-22</code>
   reproducible clean-machine packages.
8. Run <code>SHIELD-18B</code> after every retained authoritative payload owner is present.
9. Add <code>OPS-26</code> lifecycle soak after the real save/photo/network journeys exist.
10. Complete the spec-reconciliation items and decide the gameplay/authoring commitments.

### Phase 5: GPU modernization after product truth

Do not use generic cube, reflection-only shaders, or skipped parity as production closure.
Recommended order:

1. Resolve <code>GPU-P01</code>: narrow it to an honest static readiness lint or replace it
   with runtime production-pass readiness proof.
2. Complete <code>GPU-P02</code>: shipping device, presentation/swapchain, runtime backend
   selection, and a normal client frame.
3. Complete <code>GPU-P04</code>: a real semantic production shader pair through Slang,
   rather than reflection-only surrogates.
4. Complete <code>GPU-P09</code> (and retire historical <code>GPU-07</code>): prove the real
   reduced-scale TAAU/history path without disabling TAAU.
5. Reopened <code>GPU-P05</code>: run one real production pass through the intended RHI,
   folding the non-skipped production parity requirement from <code>GPU-09</code> into this
   pilot where practical.
6. Split <code>GPU-14</code> by actual capture support. Keep the real RenderDoc closure
   with its landed scope; leave PIX and Nsight in progress until each produces a real
   capture artifact. Detection and injected doubles are not closure.
7. <code>GPU-P10A</code>: wrap the already-working TAAU path behind a real
   <code>IUpscaler</code> contract/provider.
8. Complete <code>FOLIAGE-09</code> immediately before the relevant GPU-P06 compute batch:
   compile without shader-int64 and prove GPU/CPU placement-hash parity.
9. Interleave <code>GPU-P06</code> and <code>GPU-P07</code> in small semantic pass batches
   using the current Slang toolchain.
10. <code>GPU-P10B</code>: at least one real cross-vendor provider; a compiling FSR-shaped
   stub is insufficient.
11. <code>GPU-P11</code>: DLSS only after production Vulkan and the upscaler contract.
12. Timebox <code>GPU-P08</code> DX12.
13. <code>GPU-P12</code>: BLAS/TLAS.
14. Run <code>GPU-P13</code> and <code>GPU-P14</code> in parallel after BLAS/TLAS where
   safe; reflections need not depend on full RT-GI.

<code>NET-12</code> remains explicitly hardware/account blocked and outside the active
queue.

## Parallelization and ownership guidance

Parallel-safe lanes:

- Backlog/schema/validator repair versus read-only evidence-digest preparation.
- Input/persistence contracts versus visual-gate fixture design.
- UI/photo work versus bounded networking portability.
- Foliage correctness versus shader portability, if they do not touch the same render
  integration points.

Serialize:

- <code>ATMO-13</code> with Aether and Instinct changes that consume the anchor set/field
  windows.
- Server <code>OPS-16</code>, <code>INSTINCT-16</code>, then
  <code>INSTINCT-14</code> in that ownership order.
- SHIELD-18A transaction changes with payload registration, and SHIELD-18B as one final
  integration audit.
- Production RHI work with broad foliage/water render changes.
- Backlog count/rank updates so one owner performs the final atomic reconciliation.

For each implementation task, use a thinking/review agent to challenge requirements and
evidence, and a coding agent to own a bounded file/test surface. One integrator must remain
responsible for production wiring and the end-to-end proving journey. Subagents should not
independently edit <code>backlog.json</code> or <code>priority-ranking.md</code>.

## Validator requirements for the repaired backlog

Extend <code>docs/audit/021/validate_backlog.py</code> to fail on:

- Unknown status/dimensional enum values.
- Duplicate active IDs and unresolved supersede chains.
- Done items depending on todo, blocked, or in-progress items without an explicit
  justification.
- Missing evidence paths or nonexistent test/mode names.
- Proving signals that are skipped, disabled, proposed-only, or still prefixed
  <code>NEW:</code> for a done record.
- Done records without a closure note containing reviewed HEAD/date/result.
- Stale baseline hashes presented as current.
- Parent and child records both counted as independent active work.
- Empty production-wiring/reachability fields for behavior claims.
- Missing spec coverage for explicitly tracked specifications.

Static validation cannot prove that a symbol has a production caller or that a visual is
good. Those remain review obligations, but the schema should make absent proof visible.

## Evidence snapshot and limitations

Evidence reviewed during this audit:

- Current JSON backlog and priority ranking.
- Pillar audit documents under <code>docs/audit/021</code>.
- Specifications under <code>docs/specs</code>.
- Current source, data, shaders, tests, CMake registration, and validation scripts.
- Existing SHIELD-08 ratification and the 2026-07-11 nightly report.
- Existing release visual sweep montages and objective critique, treated as known-bad
  fixtures pending capture-provenance confirmation.

Other than writing this handoff document, no state-mutating, build/test, Forge, game-launch,
capture, or network command was used to generate proof. Repository inspection commands were
read-only. The nightly and visual artifacts predate this document and were inspected rather
than regenerated.

The latest full unit evidence is strong for regression health but does not erase the
delivery gaps:

- Build, all unit tests, frontier default lane, quick determinism matrix, release build, and
  render budget passed.
- One second-backend FLIP case remains skipped.
- Passing unit/integration suites do not establish normal-play activation for the false
  greens listed above.
- A passing heuristic visual report does not establish visual fidelity.

## Open decisions with recommended defaults

| Decision | Recommended default |
| --- | --- |
| How should the schema evolve? | Update the canonical schema and every consumer atomically. Preserve history in version control; do not create a parallel backlog-v2 source of truth. |
| Determinism: identical cross-build hashes or narrower compatibility contract? | Decide explicitly before persistence/network protocol work; do not infer from current per-build baselines. |
| Multiplayer ID: <code>NET-14</code> or <code>NET-UX-01</code>? | Use <code>NET-14</code> to continue the existing pillar sequence and mention the former alias in migration notes. |
| README-only gameplay/authoring claims | Remove or mark planned unless the owner recommits and creates funded backlog items. |
| Visual baseline ownership | Require named owner/art-direction approval and immutable prior references. |
| Performance floor | Name the target hardware, resolution, profile, and acceptable median/p99 before implementation. |
| Photo/save storage root | Use an application/user-data root partitioned by profile/world, never the source-tree data directory. |

## Exact next-action checklist

- [ ] Preserve the dirty worktree and stage only intentional audit files.
- [ ] Intentionally track this handoff in an owner-approved commit; it is currently a new
  untracked file.
- [ ] Plan an atomic canonical-schema and consumer update; do not create a parallel backlog.
- [ ] Apply the reopen/todo-to-in-progress corrections and ratified SHIELD-08 closure,
  recording the 160/30/0 to 144/27/19 interim ledger.
- [ ] Close OPS-09 atomically with its tracked durable digest, producing 145/26/19.
- [ ] Repair conditional done wording and split NET-11.
- [ ] Apply the GPU duplicate/supersede and Aether completion-parent maps.
- [ ] Split persistence into SHIELD-18A transaction infrastructure and SHIELD-18B final
  completeness.
- [ ] Add the Phase-0/safety-critical candidates, then decomposed foundation and
  spec-reconciliation children.
- [ ] Decompose epics before dispatch and assign effort, risk, dependencies, and individual
  proving signals.
- [ ] Resolve proposed IDs and dependencies.
- [ ] Rewrite the active priority queue using the phases in this handoff.
- [ ] Refresh metadata, baselines, pillar status tables, and ranking counts atomically.
- [ ] Harden and run the backlog validator.
- [ ] Check that every done proof is real, current, non-skipped, and appropriately scoped.
- [ ] Review the resulting diff independently before implementation dispatch.
- [ ] Continue to avoid Forge until the owner says it is ready.

## Handoff completion criterion

This handoff has been consumed successfully when the repository contains one internally
consistent, validator-clean backlog whose statuses match evidence; duplicates do not inflate
counts; missing product/system work is represented; priorities respect persistence,
determinism, input, and production wiring; visual fidelity requires human-credible proof;
and every done record can be traced to a current implementation, normal execution path, and
appropriate acceptance result.
