# Luminumbra: The Shattered Eclipse Update

**Version:** 1.0  
**Status:** Conceptual Design  
**Theme:** Cosmic Cataclysm & Aetheric Reckoning

---

## 1. Core Narrative: The Eclipse That Broke the World

In a world where light and shadow are not just elements but living forces, an ancient prophecy has come to pass.

The **Great Eclipse** — a celestial alignment of seven moons — once occurred every thousand years. It was believed to be a moment of balance: Lumin and Umbra would merge into perfect harmony.

But this time… it did not end in unity.

Instead, the eclipse fractured reality. A shard of the Void tore through the sky, scattering **Shadowglass** across the land. The Aetheric Field is now unstable — light flickers unpredictably, magic runs wild, and creatures are driven mad by conflicting energies.

This update introduces a new world state: **The Shattered Eclipse**, where every system in Luminumbra must adapt to chaos.

---

## 2. Thematic Changes

### 2.1. Visual Aesthetic

- Sky turns deep violet with black stars.
- Light rays are jagged, breaking into shards.
- Ground cracks emit faint red and blue light — the "Echoes of the Eclipse."
- All foliage appears warped: trees grow upside-down or in spirals.

### 2.2. Sound Design

- Ambient music becomes dissonant: major chords mixed with minor.
- Wind carries whispers from the Void (low-frequency audio pulses).
- Creatures emit distorted vocalizations when near Shadowglass.

### 2.3. Lore & World State

- New NPC factions:
  - **The Lightbound**: Seek to restore balance by purifying Shadowglass.
  - **The Umbra Wyrms**: Embrace the chaos — their bodies grow darker, more powerful.
- A new world-wide event: every 10 in-game minutes, a "Eclipse Pulse" occurs — all light dims for 3 seconds, then explodes back with doubled intensity.

---

## 3. New Systems & Mechanics

### 3.1. Shadowglass (New Resource)

| Property              | Behavior                                                                            |
| --------------------- | ----------------------------------------------------------------------------------- |
| **Spawn**             | Appears after Eclipse event; grows over time in affected zones                      |
| **Interaction**       | Can be mined, but risk of "corruption" if not handled properly                      |
| **Corruption Effect** | Entities exposed to high levels lose stability: AI goes rogue, world state glitches |

### 3.2. Aetheric Surge (New State)

- The Aetheric Field now oscillates between two states:
  - **Lumin Surge**: Light intensifies — crystals glow brighter; some creatures become aggressive.
  - **Umbra Surge**: Darkness spreads — visibility drops; plants wither.

> These are not random — they follow a predictable cycle based on time of day and moon phase.

### 3.3. The Eclipse Pulse (Global Event)

- Every 10 minutes:
  1. All light sources dim to zero.
  2. A low-frequency pulse is broadcast across the world (audio + vibration).
  3. After 3 seconds, all light returns — but with **+50% intensity** and randomized direction.

> This forces players to adapt: hide during the dark, or risk being blinded when it returns.

---

## 4. Module-Based Architecture Enhancement

Luminumbra is now structured as a set of pluggable modules. Each update adds new behavior through modular systems — no monolithic codebase.

### Core Modules (Current)

| Module                | Purpose                                   |
| --------------------- | ----------------------------------------- |
| `shard_core`          | Base ECS, input handling, simulation loop |
| `shield_world`        | Voxel generation, chunk streaming         |
| `instinct_ai`         | GOAP AI system, needs & actions           |
| `atmospheric_weather` | Wind, rain, seasonal cycles               |
| `aetheric_field`      | Light energy flow, emission, diffusion    |

### New Modules for This Update

#### `eclipse_system`

- Manages the Eclipse Pulse cycle.
- Triggers global Aetheric Surge state changes.
- Handles corruption effects on entities.

#### `shadowglass_module`

- Spawns and grows Shadowglass in zones.
- Provides mining mechanics via player interaction.
- Applies "corruption" debuffs to affected entities.

#### `visual_reckoning`

- Controls the fractured light, sky color shifts, and screen distortions.
- Uses post-processing shaders with dynamic parameters.

#### `audio_choir`

- Manages dissonant ambient music.
- Implements spatialized whispers via directional audio.

---

## 5. Optimization Strategies by Module

| Module                   | Optimization Technique                                                  | Benefit                                                      |
| ------------------------ | ----------------------------------------------------------------------- | ------------------------------------------------------------ |
| **`shield_world`**       | Use octree-based chunk culling; only render visible chunks.             | Reduces draw calls by ~60% in dense areas                    |
| **`eclipse_system`**     | Event scheduling via priority queue (only one active pulse).            | Prevents tick spam during high-frequency events              |
| **`shadowglass_module`** | Store spawn zones as sparse bitmaps; update only on change.             | Reduces memory usage by 70% vs. full grid storage            |
| **`visual_reckoning`**   | Use GPU compute shaders to simulate light fracturing (no CPU overhead). | Enables real-time visual chaos with minimal performance cost |
| **`audio_choir`**        | Pre-cache audio clips; use streaming for long whispers.                 | Prevents stutter during Eclipse Pulse                        |

---

## 6. Developer Integration

- All new modules are loaded via `ModuleManager.h`.
- Use Lua scripts in `scripts/common/modules/` to enable/disable modules.
- Example: `require("modules/eclipse_system").enable()`.

> This modular design allows you to test, disable, or replace any system independently — perfect for solo dev iteration.

---

## 7. Final Note

The Shattered Eclipse is not just a cosmetic update — it’s a **systemic transformation** of the entire world engine. It pushes every module to its limits and forces optimization under stress.

> "Chaos is the true test of beauty."
