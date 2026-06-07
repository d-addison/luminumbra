# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Summary

Luminumbra is a voxel-based game engine ("The Quantum Engine") designed to create procedurally generated worlds with dynamic simulation systems. The engine emphasizes performance optimization, deterministic simulation, and an "Ethereal Voxel" aesthetic, built on an Entity Component System (ECS) architecture using EnTT. It features real-time world streaming, physics simulation, atmospheric effects, and a unique "Aetheric Field" magic system for light/shadow energy flow.

## Build Commands

```bash
# Configure build (from project root)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# Build the project
cmake --build build

# Run tests
cd build && ctest
# Or run specific test
./build/bin/world_generation_test

# Main executable
./build/bin/luminumbra_client_app
```

## Core Systems Overview

### 1. **SHIELD Engine** (World Generation & Streaming)
- Location: `src/luminumbra_common/systems/SHIELD_WorldSystem.*`
- Generates procedural voxel worlds using SDFs (Signed Distance Fields)
- Implements chunk-based streaming with LOD system
- Near-field (<256m): Marching Cubes mesh generation
- Far-field (>256m): Direct SDF ray-tracing
- Uses FastNoiseLite for procedural generation

### 2. **Rendering Pipeline**
- Location: `src/luminumbra_client/rendering/`
- Hybrid rasterization and ray-tracing approach
- Deferred rendering with G-Buffer implementation
- Cascaded shadow mapping (4 cascades)
- Point lights and directional lighting
- Custom shader system with hot-reload capability

### 3. **Physics System** 
- Location: `src/luminumbra_common/systems/PhysicsSystem.*`
- Built on Jolt Physics for deterministic simulation
- Fixed tick rate: 30 Hz
- Handles collision detection and rigid body dynamics

### 4. **Water System**
- Location: `src/luminumbra_common/systems/WaterSystem.*`
- Grid-based water simulation (8x8 resolution per chunk)
- Flow dynamics and visual rendering

### 5. **Audio System**
- Location: `src/luminumbra_client/audio/`
- MiniaudioManager implementation
- Component-based audio with spatial sound

### 6. **Entity Component System (ECS)**
- Built on EnTT library
- Components: `src/luminumbra_common/components/`
  - CoreComponents.h: Transform, Velocity, etc.
  - AudioComponents.h: Sound emitters
  - InstinctComponents.h: AI behavior
  - LightingComponents.h: Light sources
  - WaterComponents.h: Water simulation
- Data-oriented design for cache efficiency

### 7. **Scripting System**
- Location: `src/luminumbra_common/scripting/`
- Lua integration via sol2
- Scripts in `scripts/` directory:
  - AI actions and agents
  - Entity archetypes (JSON)
  - Game logic directives

## Key Data Structures

### Core Types (`include/luminumbra/core/Types.h`)
- **Vector types**: Vec2, Vec3, Vec4 (GLM aliases)
- **Matrix types**: Mat3, Mat4
- **Entity IDs**: EntityID (entt::entity), ChunkID (uint64_t)
- **Material types**: Enum defining Air, Stone, Soil, Grass, etc.

### World Constants
- **Chunk dimensions**: 16x16x16 voxels
- **Tick rate**: 30 Hz fixed timestep
- **Render distances**: Near field 256m, Far field 8192m
- **Sea level**: 0.0f

### Chunk System (`src/luminumbra_common/world/Chunk.*`)
- Voxel storage with bit-packed arrays
- LZ4 compression for disk I/O
- Marching Cubes mesh generation

## Architectural Pattern Analysis

The engine follows a **hybrid ECS/System architecture**:

1. **Entity Component System (ECS)**: Core game logic uses EnTT for data-oriented design
2. **System-based Updates**: Fixed-tick deterministic simulation at 30 Hz
3. **Job System**: Parallel processing via `JobSystem` class for threading
4. **Event Bus**: Component communication via event system
5. **Client-Server Split**: 
   - `luminumbra_common`: Shared simulation code
   - `luminumbra_client`: Rendering, audio, UI (RmlUi)
   - `luminumbra_server`: Host authority logic

## Development Workflow

### Hot-Reload Systems
- Lua scripts can be modified at runtime
- Shader files support hot-reload during development

### Testing Framework
- Google Test integration
- Test files in `test/` directory
- Focus on deterministic simulation verification

### Asset Pipeline
- Raw assets in `assets/` (.glb files)
- Asset processor converts to `.lmesh` format
- Processed data in `data/` directory

## Third-Party Dependencies

- **EnTT**: Entity Component System
- **Jolt Physics**: Deterministic physics
- **GLFW/GLAD**: Window management and OpenGL
- **GLM**: Mathematics library
- **RmlUi**: HTML/CSS UI system
- **ImGui**: Debug UI and tools
- **Lua/sol2**: Scripting
- **Miniaudio**: Audio playback
- **FastNoiseLite**: Procedural generation
- **LZ4**: Compression
- **nlohmann/json**: JSON parsing
- **spdlog**: Logging

## Suggestions for Next Steps

1. **Instinct Engine Implementation**: The AI system referenced in documentation needs core implementation in `src/luminumbra_common/` - currently only Lua scripts exist
2. **Aetheric Field System**: The magic/energy system described in docs needs implementation - no core C++ code found for this feature