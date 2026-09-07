# Engine, modules, and game ownership

Status: accepted architectural direction, September 7, 2026. Extraction and the
authoring overhaul are planned. Current game code still lives in this engine
repository. Foundations implementation follows independently verified v0.3.0
publication, refreshed architecture review, and the single roadmap approval.

Luminumbra provides a small core and substantial optional reusable modules. The
separate [game repository](https://github.com/d-addison/luminumbra-game) owns game
rules, authoritative game components, composition, content and presentation.

| Layer | Responsibility |
|---|---|
| Engine core | Lifecycle, stable identities, ECS services, jobs, deterministic scheduling/RNG, registration, canonical encoding, commands and diagnostics |
| Optional engine modules | Rendering, physics/character movement, animation, voxel storage/generation/streaming, water/weather and field solvers, audio, UI/input, scripting, persistence/network/replay mechanisms, reusable AI and behavior facilities |
| Game | Species/ecosystem rules, diets and reproduction conditions, farming yields, photography/codex/objectives/difficulty, aether meaning, abilities, authored biomes/materials/presets, screens/HUD, soundscapes and the composition of selected modules |

Aether is a game implementation of a reusable field concept. An optional engine
field module can supply storage, sampling, diffusion, decay and generic source/sink
mechanisms. The game defines channel meanings and units, where and when emission
occurs, creature/material consequences, and the blue-violet visual/audio treatment.
The same service must work for a non-aether example such as heat or pollution.

Reusable behavior facilities can provide attributes/resource meters, timers,
lifecycle/growth mechanisms, perception and AI decisions. The game defines hunger
rates and consequences, diets, prey selection, crop stages/yields and reproduction
rules. A machine battery is a useful contrasting consumer of resource/decision
facilities. Adding configuration knobs alone does not establish reuse.

Physics accepts neutral commands and properties at declared phases; the game owns
abilities and their causal decisions. Terrain can reference registered opaque
material IDs and generic physical/render properties; the game owns named identities,
biomes and current-format compatibility mappings. Feedback between simulation
modules and game reactions follows explicit deterministic scheduling and command
contracts.

## Authoring without rebuilding the engine

The desired authoring model is scripts, components and data over an installed
engine. Meaningful game-rule/content iteration must not require engine C++ edits or
recompilation. Native engine and game modules remain available for demanding
mechanisms. This does not imply arbitrary native ABI hot reload or a wholesale
rewrite of current mechanics into scripts.

Design the script/component contracts before extraction: typed registered state,
commands and queries; lifecycle/service discovery; separate authoritative and
presentation contexts; deterministic scheduling/RNG; execution/resource bounds;
error handling; and explicit content/script/schema compatibility before restore or
network admission. Reuse the existing Lua infrastructure where justified by the
runtime review. Its proposed API manifest is not evidence that every binding exists.
Script or content changes may require a controlled session restart; live reload
must have an explicit state and compatibility contract before it is advertised.

## Acceptance

- Engine-only graphical and headless hosts build/run with game code/content absent.
- A separate native consumer uses installed targets and public headers through
  `find_package(Luminumbra CONFIG REQUIRED)`.
- A substantially different script/component/data example runs using unchanged
  installed engine binaries, with no private includes or dummy game assets.
- The separately composed game preserves populated client/server/save/replay
  behavior, full host tick order (including outer physics), configuration positions,
  hash/state contributions and supported current-format bytes.
- CI enforces target/header/schema/content dependency direction; registered UI,
  commands, inspectors and render/audio adapters have safe lifetimes and authority.

Preserve preset revision 6, LMR1 v2, FSD2 payload v3, canonical in-memory
serialization, terrain-quality assertions and default-off policies. No obsolete
world migration or legacy loading is introduced. A necessary format change requires
separate explanation and review, not altered expectations hidden in extraction.

See the [coordinated roadmap](roadmap.md) for dependencies, ownership, acceptance
issues and provisional release allocations.
