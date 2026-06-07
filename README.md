# Project: Luminumbra

The "Quantum" Engine. See docs/TDD.md for the full technical design.

---

## 1. Core Philosophy: The Living Diorama

Luminumbra is not a game — it is a living world engine. Its purpose is to create an experience where the world feels both **tangibly constructed** at the micro level and **vastly alive** at the macro level. Every tree, every creature, every ray of light must feel intentional.

This vision is driven by two principles:

- **Performance by Design**: The engine is built from the ground up to run efficiently on consumer hardware while supporting massive scale.
- **Beauty Through Optimization**: Visual fidelity and technical elegance are not separate — they are unified. The "Ethereal Voxel" aesthetic emerges directly from performance constraints.

The world is a dynamic system where every element interacts with the others through physics, energy flow, and behavior. It evolves over time, shaped by weather, player actions, and emergent AI.

---

## 2. The Four Pillars of the Engine

### 2.1. The SHIELD Engine (The World)

**Purpose**: Generate, stream, render, and persist a procedurally constructed world with infinite detail.

- **Foundation**: Voxel-based geometry using SDFs (Signed Distance Fields) for smooth, scalable terrain.
- **SDF Contract**: `docs/shield/sdf-contract.md` defines the density sign convention, chunk sample layout, isolevel, and CPU/GPU generation obligations used by SHIELD.
- **Procedural Generation**: Worlds are seeded from a single integer. The same seed produces identical worlds across all platforms.
- **Streaming & LOD**:
  - Chunks are loaded dynamically based on camera position.
  - Near-field (<256m): High-detail polygonal meshes generated via Marching Cubes with dual contouring for smoothness.
  - Far-field (>256m): Ray-traced directly against SDF data — infinite view distance with parallax and perfect detail.
- **Memory Optimization**:
  - Voxel storage uses bit-packed arrays (4 bits per voxel).
  - Chunk data is compressed using LZ4 before disk I/O.
  - Only active chunks are loaded into memory; others are swapped out.

> The world is not stored as static meshes — it is computed on-demand from math and noise.

### 2.2. The Instinct Engine (The Will)

**Purpose**: Simulate self-motivated, goal-driven wildlife whose behavior emerges from needs and sensory input.

- **Architecture**: Data-driven GOAP (Goal-Oriented Action Planning) with a finite state machine.
- **Core Concepts**:
  - **Needs**: Hunger, Thirst, Fatigue, Safety, Curiosity. Each has a threshold that triggers action.
  - **Senses**: Sight, Sound, Smell, Touch. These detect resources, danger, and other entities.
  - **Actions**: Move, Eat, Drink, Rest, Explore, Flee. Actions have cost (energy) and duration.
- **Emergent Behavior**:
  - A creature with high hunger will seek food, but only if it doesn’t sense danger nearby.
  - A young animal may wander far from its parent to explore — a survival trade-off.
- **Determinism**: All decisions are based on deterministic inputs (world state, entity data). No randomness in behavior logic.

### 2.3. The Atmospheric Engine (The Breath)

**Purpose**: Simulate dynamic weather and seasonal cycles that affect gameplay and visuals.

- **Weather System**:
  - Rain, wind, snow, fog, storms.
  - Generated via perlin noise fields modulated by terrain height and season.
  - Wind affects foliage, particle systems, and AI movement (e.g., fleeing from storm).
- **Seasons & Climate Zones**:
  - Four seasons: Spring, Summer, Autumn, Winter.
  - Each biome has a seasonal cycle based on latitude and altitude.
  - Seasonal changes affect plant growth, animal migration, and resource availability.
- **Performance Optimization**:
  - Weather state is updated only once per tick (30 Hz).
  - Wind fields are precomputed in a grid to avoid real-time calculations.

### 2.4. The Aetheric Field (The Magic)

**Purpose**: Represent the flow of magical energy — Lumin (light) and Umbra (shadow) — as a unified, persistent data field.

- **Core Mechanics**:
  - Energy flows from sources (crystals, sun, moon) through terrain.
  - Entities absorb or emit energy based on material properties.
  - The Aetheric Field is updated every tick via diffusion algorithm.
- **Visual Impact**:
  - Lumin emits soft glow; Umbra creates deep shadows.
  - Subsurface scattering makes crystals and flora appear to glow from within.
  - Radiance Cascade Global Illumination provides realistic light bouncing.
- **Gameplay Integration**:
  - Players can manipulate energy flow with tools (e.g., redirecting a crystal’s beam).
  - Certain items only activate in high Lumin or Umbra zones.

---

## 3. Core Architecture: Deterministic ECS & Asynchronous Jobs

### 3.1. Entity Component System (ECS) with EnTT

- **Rationale**: Achieve maximum performance and cache coherency.
- **Structure**:
  - Entities are unique IDs; components are data-only structs stored in contiguous arrays.
  - Systems iterate over component arrays — not individual entities.
- **Performance Benefits**:
  - Memory layout is ideal for CPU caching.
  - No virtual function overhead.
  - Systems can be parallelized safely using job queues.
- **Implementation**:
  - `luminumbra_common/components/` contains all C++ component definitions (e.g., `Transform`, `Burning`, `Needs`).
  - Lua scripts define archetypes via JSON — used to spawn entities.

### 3.2. Deterministic Fixed-Tick Simulation

- **Tick Rate**: 30 ticks per second.
- **Synchronization Model**: Input-based replication using Steam Networking.
  - Only player inputs are sent to the host.
  - The host broadcasts bundled inputs every tick.
  - Every client runs the same simulation with identical input — ensuring bit-for-bit consistency.
- **Advantages**:
  - Minimal bandwidth (only ~10–20 bytes per player per tick).
  - No state reconciliation or rollback logic required.
  - Full replayability: record a session, rewind it, debug step-by-step.

---

## 4. The Rendering Pipeline: "Ethereal Voxel" Aesthetic

### 4.1. Hybrid Rasterization & Ray-Tracing (SHIELD-RT)

| Layer | Technique | Purpose |
|------|----------|--------|
| Near-Field (<256m) | Dual-contoured polygonal meshes rendered into a G-Buffer | High detail, lighting accuracy |
| Far-Field (>256m) | Real-time ray-tracing against SDF data | Infinite view distance with parallax |

**Optimization Techniques**:
- Ray tracing uses early termination and spatial partitioning (BVH).
- Only visible rays are traced; others are culled via frustum and occlusion.
- Ray output is cached per frame to avoid redundant calculations.

### 4.2. "Aether-Infused" Lighting

- **Radiance Cascade Global Illumination**:
  - Three cascades: near, medium, far.
  - Each cascade uses a lower-resolution shadow map.
  - Bounces are computed in real time with minimal cost.
- **Material Properties**:
  - Emissive materials (crystals, fungi) glow based on Aetheric Field strength.
  - Subsurface scattering simulates light penetration through surfaces.

### 4.3. "Living Diorama" Foliage

- All plants are rendered as instanced 3D geometry — no alpha cards.
- Animation is GPU-driven: vertex shaders respond to wind and player proximity.
- Wind force is calculated per grid cell, then passed to the shader via uniform buffer.

---

## 5. World Generation & Persistence

### 5.1. Two Modes of World Creation

| Mode | Description |
|------|-----------|
| **Seeded Random** | Generate a unique world from a user-provided integer seed. Ideal for exploration and sharing. |
| **World Painter (Hand-Crafted)** | Use the in-game editor to design custom biomes, place structures, spawn creatures, and define initial Aetheric Field state. |

- Both modes use the same underlying SHIELD engine — ensuring consistency.
- Hand-crafted worlds can be saved as `.lworld` files with metadata (seed, author, version).

### 5.2. Save & Load System

- Worlds are saved in a binary format using `nlohmann/json` for metadata and custom serialization for voxel data.
- Compression: LZ4 is applied to save files (~70% reduction).
- Integrity checks ensure no corruption during load.

---

## 6. Key Items & Mechanics

### Core Item Types

| Type | Function |
|------|--------|
| **Lumin Crystal** | Emits light; can be used to power devices or attract creatures. |
| **Umbra Shard** | Absorbs light; useful for stealth, creating shadows. |
| **Wind Bell** | Generates wind pulses — affects foliage and AI behavior. |
| **Seed Pod** | Plantable item that grows into a tree after 30 seconds of sunlight exposure. |

### Interaction System

- All items interact with the Aetheric Field.
- Players can collect, craft, and place items via hotkeys or UI.
- Crafting requires specific materials in correct ratios — enforced by Lua scripts.

---

## 7. Directory & File Structure

This structure enforces separation between engine (C++), game logic (Lua), data, and tools.

```
luminumbra/
├── assets/                  # Raw source art (.blend, .psd)
├── build/                   # Build output (ignored)
├── data/                    # Processed, hot-reloadable data (.ktx, .lmesh)
├── scripts/                 # Lua gameplay logic
│   ├── client/              # UI, camera effects
│   ├── common/              # Core systems: archetypes, AI actions, GOAP agents
│   └── server/              # Host-only: admin commands, difficulty tuning
├── tools/                   # Standalone utilities (e.g., asset processor)
├── worlds/                  # Atlas graphs and player saves (.lworld files)
├── src/
│   ├── luminumbra_client/   # Presentation layer (rendering, audio, UI)
│   ├── luminumbra_common/   # Core simulation: ECS, networking, systems
│   └── luminumbra_server/   # Host authority logic
├── vendor/                  # Third-party libraries
└── CMakeLists.txt           # Build configuration
```

---

## 8. Key Third-Party Libraries

| Library | Purpose |
|--------|--------|
| **EnTT** | ECS core — performance-critical, cache-friendly design |
| **Jolt Physics** | Deterministic, multithreaded physics (collision, gravity) |
| **Lua + sol2** | Hot-reloadable scripting for gameplay logic and AI |
| **GLFW, GLAD, GLM** | Graphics, math, and windowing foundation |
| **RmlUi** | HTML/CSS-based UI system — supports custom themes and dynamic content |
| **ImGui** | In-editor tools: World Editor, profiler, debug HUDs |
| **nlohmann/json** | Data parsing for archetypes, save files, configuration |
| **Steam Networking SDK** | P2P multiplayer with low latency, reliable input sync |

---

## 9. Developer Workflow

- The in-game World Editor is the primary tool — all changes are visible instantly.
- All AI decisions and state transitions are logged to a `DebugLogComponent` for inspection.
- Performance metrics (frame time, memory usage) are measured daily via automated benchmarks.
- A built-in assertion system provides context-rich error messages during development.

---

## 10. Final Note

Luminumbra is not just an engine — it is a **world-making platform**. Every choice in architecture serves two goals: **optimization** and **beauty**. The result is a living, breathing world that feels both real and magical — where every voxel matters.

> "The universe is made of stories, not atoms."
