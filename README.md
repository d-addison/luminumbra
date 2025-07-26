# project-capture

***

# Project: Luminumbra
![Status: In Development](https://img.shields.io/badge/Status-In%20Development-blue) ![Version: 1.1](https://img.shields.io/badge/Version-1.1-brightgreen) ![Language: C++17](https://img.shields.io/badge/Language-C%2B%2B17-blueviolet) ![Build: CMake](https://img.shields.io/badge/Build-CMake-red) ![Graphics: OpenGL 3.3+](https://img.shields.io/badge/Graphics-OpenGL%203.3%2B-orange)

**Luminumbra** is a "zen" photography and exploration game built from the ground up on a custom C++ engine. This document serves as the comprehensive Game Design & Technical Specification, acting as the central source of truth for all development efforts.

---

## Table of Contents
1.  [**Vision & Concept**](#1-vision--concept)
    *   [1.1 Logline](#11-logline)
    *   [1.2 Core Pillars](#12-core-pillars)
    *   [1.3 Genre & Target Audience](#13-genre--target-audience)
2.  [**Gameplay Mechanics**](#2-gameplay-mechanics)
    *   [2.1 Core Gameplay Loop](#21-core-gameplay-loop)
    *   [2.2 The Player's Kit](#22-the-players-kit)
    *   [2.3 The Codex & Progression System](#23-the-codex--progression-system)
3.  [**World & Art Direction**](#3-world--art-direction)
    *   [3.1 Visual Identity](#31-visual-identity)
    *   [3.2 The Shifting Isles: World Structure](#32-the-shifting-isles-world-structure)
    *   [3.3 Dynamic Color Palettes](#33-dynamic-color-palettes)
    *   [3.4 Biomes & Environments](#34-biomes--environments)
4.  [**Creatures & AI System**](#4-creatures--ai-system)
    *   [4.1 AI Architecture Philosophy](#41-ai-architecture-philosophy)
    *   [4.2 MVP Creature Roster & Technicals](#42-mvp-creature-roster--technicals)
5.  [**Audio Design**](#5-audio-design)
    *   [5.1 Sound Design Philosophy](#51-sound-design-philosophy)
    *   [5.2 Dynamic Music System: Technicals](#52-dynamic-music-system-technicals)
6.  [**User Interface (UI/UX)**](#6-user-interface-uiux)
    *   [6.1 Design Philosophy: Diegetic & Minimal](#61-design-philosophy-diegetic--minimal)
    *   [6.2 Key UI Components](#62-key-ui-components)
7.  [**Engine & Technical Specification**](#7-engine--technical-specification)
    *   [7.1 Core Architecture & Language](#71-core-architecture--language)
    *   [7.2 Rendering Pipeline Deep Dive](#72-rendering-pipeline-deep-dive)
    *   [7.3 World Generation: "Echo LOD" System](#73-world-generation-echo-lod-system)
    *   [7.4 Physics & Player Controller](#74-physics--player-controller)
8.  [**Development & Tooling**](#8-development--tooling)
    *   [8.1 Source Control & Build System](#81-source-control--build-system)
    *   [8.2 Code Architecture & Philosophy](#82-code-architecture--philosophy)
    *   [8.3 Asset Pipeline & Tooling](#83-asset-pipeline--tooling)
    *   [8.4 In-Game Development Tools](#84-in-game-development-tools)
9.  [**Development Roadmap**](#9-development-roadmap)
    *   [9.1 Phase Breakdown](#91-phase-breakdown)
    *   [9.2 Minimum Viable Product (MVP) Definition](#92-minimum-viable-product-mvp-definition)
    *   [9.3 Post-Launch & Stretch Goals](#93-post-launch--stretch-goals)
10. [**Document Control**](#10-document-control)

---

## 1. Vision & Concept

### 1.1 Logline
> In a world painted with impossible light and living shadows, you are a field researcher armed with a unique camera. Your mission is to document the elusive and magical creatures that transform between the realms of light and dark.

### 1.2 Core Pillars
*   **Atmospheric Immersion:** The world is the primary character. Every sight and sound is designed to evoke a sense of wonder, peace, and discovery.
*   **Patient Observation:** A non-violent, "zen" gameplay experience focused on watching, waiting, and understanding the environment to capture the perfect moment.
*   **Creative Expression:** A deep but accessible photography system that allows players to frame beautiful, artistic shots.
*   **A Living Ecosystem:** The world and its creatures operate on dynamic cycles, reacting to the environment and the player's presence.

### 1.3 Genre & Target Audience
*   **Genre:** Exploration, Simulation, "Zen" Photography Game.
*   **Target Audience:** Players who enjoy non-combat exploration games like *Journey*, *ABZÛ*, and *Outer Wilds*, or creative simulations like *Pokémon Snap* and *Alba: A Wildlife Adventure*.

## 2. Gameplay Mechanics

### 2.1 Core Gameplay Loop
The player experience revolves around a simple, rewarding cycle:
1.  **Explore:** Traverse biomes using fluid, ground-based movement (walk, sprint, jump, light mantle/climb).
2.  **Observe:** Identify creature habitats and environmental cues. Watch their behaviors, paying close attention to their interaction with the world's shifting light and shadow.
3.  **Interact & Capture:** Use tools to manipulate light/shadow, triggering unique creature behaviors. Use the camera to document these moments.
4.  **Catalog:** Return to Base Camp to submit photos to the Codex. High-quality photos yield research points, unlocking gear, new areas, and narrative fragments.

### 2.2 The Player's Kit

#### 2.2.1 The Camera
The primary tool. Accessible via a dedicated button that shifts to a first-person viewfinder mode with a smooth transition.
*   **Lenses (Unlockable):**
    *   **Standard Lens (50mm):** All-purpose starting lens. Moderate zoom and DoF.
    *   **Telephoto Lens (200mm):** For shy creatures. High zoom, compressed depth.
    *   **Macro Lens:** For extreme close-ups of flora and small fauna. Creates extreme bokeh.
    *   **Aether Lens:** Reveals faint, glowing trails of recent creature activity and environmental "memories." `Tech Note: Renders a secondary pass of particle effects and decals normally culled from view.`
*   **Controls & Technicals:**
    *   **Focus:** Manual or automatic. `Tech Note: Implemented in a post-processing shader, using the depth buffer to determine blur strength based on a focal plane uniform.`
    *   **Aperture (DoF):** Controls depth of field. `Tech Note: Lower F-stop values will increase the blur intensity in the DoF shader.`
    *   **Shutter Speed:** Controls motion blur. `Tech Note: Slow shutter speeds will require accumulating the scene in a framebuffer over several frames, blending them to create light trails.`

#### 2.2.2 Player Tools
*   **Glimmer-stone:** Throwable. Creates a temporary (15s) pool of "Lumin" light with a 5m radius. Player can carry a maximum of 3, regenerates at Base Camp. `Tech Note: Spawns a dynamic point light with a specific color temperature and falloff.`
*   **Shade-moss:** Throwable. Creates a temporary (20s) patch of deep "Umbra" shadow with a 4m radius. `Tech Note: Spawns a 'negative light' or a screen-space decal that darkens the underlying surfaces and overrides the global directional light's influence in that area.`

#### **2.2.3 Exploration & Traversal Gear**
These tools are essential for navigating the Shifting Isles and setting up the perfect photograph. They are unlocked through the bartering system.

*   **Trail Markers:**
    *   **Description:** A set of small, reusable stakes topped with a strip of high-visibility cloth. The player can place these in the world to mark paths, points of interest, or the boundaries of a creature's territory.
    *   **Functionality:** The player has a limited number (e.g., 20) they can place. Placing a new one beyond the limit will despawn the oldest one. They can be picked up manually.
    *   **Aether Lens Synergy:** When viewed through the **Aether Lens**, the cloth on the markers glows brightly, making them visible from extreme distances and through foliage, allowing the player to retrace their steps or align a long-distance shot.
    *   **Upgrade:** Linnea the Botanist can provide a recipe to coat the markers in a bioluminescent paste, making them glow faintly in Umbra zones even without the Aether Lens.
    *   `Technical Note: Implemented as a simple, placeable static mesh actor. A manager class tracks the placed markers in a queue for the despawning logic.`

*   **Field Campfire Kit:**
    *   **Description:** A single-use, deployable kit that creates a small, contained campfire. It serves as a temporary source of warmth, light, and safety.
    *   **Functionality:** Creates a significant pool of "Lumin" light (~8m radius), more powerful and longer-lasting (5 minutes) than a Glimmer-stone. It can be used to create a large, stable area to observe or trigger creature transformations. Some skittish creatures will avoid the fire, while some curious or cold-blooded ones might be drawn to it.
    *   **Limitations:** The player can only carry one kit at a time. They are crafted at the Base Camp using gathered resources (e.g., Elderwood Scraps, Flint).
    *   `Technical Note: A placeable actor that spawns a point light with a flickering effect, a particle system for fire/smoke, and an audio component for crackling sounds. It has a visible timer (the wood pile diminishes) before it extinguishes.`

*   **Ascender & Line:**
    *   **Description:** A sophisticated piece of climbing gear consisting of a handheld launching mechanism that fires a retrievable piton, attached to a high-tensile spool of rope. This is the key to verticality and reaching ideal vantage points.
    *   **Functionality:** The tool has two primary modes:
        1.  **Vertical Ascent/Descent:** The player aims at a valid surface (packed earth, wood, non-brittle rock) within a certain range (e.g., 30m). Firing attaches the piton. The player can then ascend or descend the rope at a controlled speed. The rope can be recalled from either the top or the bottom.
        2.  **Zipline (Line Traverse):** After firing the first piton, the player can aim at a *second* valid point (that is lower than the first) to create a zipline for rapid horizontal and downward movement. This is perfect for crossing chasms or quickly moving between observation spots.
    *   **Limitations:** Will not attach to unsuitable surfaces like slick crystal, loose scree, or ethereal/magical flora. There is a maximum rope length.
    *   `Technical Note: The firing mechanism uses a raycast to check for valid surface tags within range. If successful, it instantiates an anchor point. The rope itself is a procedurally generated mesh (a simple cylinder or a series of quads) that is dynamically scaled and oriented between the player and the anchor. For ziplines, it's stretched between two anchor points.`

## 2.3 Interaction

#### **2.3 The Settlement, Bartering, & World Progression**

The addition of new gear enriches the bartering system and gives the player more compelling rewards to work towards.

*   **Key NPCs & Their Roles (Updated Rewards):**
    *   **Elara, the Lead Researcher:** Continues to provide main story assignments. Rewards may now include narrative items or access to entirely new islands.
    *   **Kael, the Guild Quartermaster:** He is the primary source for major equipment upgrades.
        *   *Initial Request:* "I need proof a Grove-Strider is as shy as the old tales say. Get me a photo from at least 100m away." -> **Reward: Telephoto Lens.**
        *   *Later Request:* "The cliffs on the north side are too steep for our scouts. Map a safe route up by getting me a photo from the summit." -> **Reward: The Ascender & Line.**
    *   **Linnea, the Botanist:** She focuses on consumable and flora-related upgrades.
        *   *Request:* "Show me a Sun-petal flower at its most open point in the full light." -> **Reward: Recipe for an upgraded Glimmer-stone (brighter or longer duration).**
        *   *Request:* "The Umbra spores are beautiful, but I need to know what they grow from. Get me a macro shot of a Moonpetal fungus." -> **Reward: Recipe for Bioluminescent Trail Marker paste.**
    *   **Roric, the Artisan:** His requests lead to quality-of-life and cosmetic rewards.
        *   *Request:* "I saw a Skitter-Sprite collecting pebbles by the creek. I want to see its hoard!" -> **Reward: An expanded pack, allowing you to carry more Campfire Kits or other resources.**
        *   *Request:* "Capture the 'fire-dance' of a Flutterwing as it transforms. I have an idea for a new carving." -> **Reward: A beautiful cosmetic customization for your camera or player journal.**

#### 2.3.1 Philosophy: A Diegetic Economy of Discovery
We are abandoning abstract scoring and point-based progression. Instead, the player's discoveries—their photographs—become a valuable resource within a living community. Progression is driven by fulfilling the needs and curiosities of fellow inhabitants, making the player an integral part of their settlement's growth and culture. The core loop is not about pleasing a menu, but about contributing to a community.

#### 2.3.2 The Hub: The Base Camp Settlement
The player is not alone. The Base Camp is a small, growing settlement of researchers, artisans, and explorers, each with their own personality, needs, and expertise. These NPCs are the heart of the progression system.

*   **Key NPCs & Their Roles:**
    *   **Elara, the Lead Researcher:** An older, wise woman who provides the main story-driven research assignments. She is interested in the "big picture"—the nature of the Lumin/Umbra duality.
    *   **Kael, the Guild Quartermaster:** A practical, grizzled explorer who manages the settlement's gear. He barters for photos that prove the utility or danger of a region, and is the primary source for new lenses and core equipment.
    *   **Linnea, the Botanist:** A quiet, observant woman fascinated by the flora. She requests macro shots of plants in their different states and rewards the player with upgrades to their consumable tools (`Glimmer-stones`, `Shade-moss`).
    *   **Roric, the Artisan:** A craftsman who finds inspiration in the world's creatures. He requests aesthetically interesting photos (unique poses, beautiful lighting) to inspire his work (pottery, carvings, weavings). His rewards are often cosmetic or quality-of-life upgrades for the Base Camp.

#### 2.3.3 The Request Board & Bartering System
The primary interaction mechanism is the **Settlement Request Board**, a physical object in the center of the camp.
*   **How it Works:** NPCs will post handwritten requests on the board. Each request specifies:
    1.  **The Subject:** (e.g., a Grove-Strider)
    2.  **The Condition:** (e.g., "while it is performing its transformation," or "a close-up of its glowing Umbra patterns.")
    3.  **The Reward:** (e.g., "A pristine Telephoto Lens," or "Recipe for Extended-Duration Shade-moss.")
*   **Fulfilling Requests:** The player "pins" a request to their active objectives. When they believe they have captured the required photo, they present it to the NPC who made the request.
*   **Photo Metadata:**
    *   `Technical Note:` For this system to work, every photo taken must save a small packet of metadata alongside the image file. This includes: `timestamp`, `biomeID`, `subjectID(s)`, `subjectState(s)` (Lumin, Umbra, Fleeing, Transforming), `distanceToSubject`, `lensUsed`.
    *   When a photo is presented, the system checks this metadata against the request's conditions. This is a binary check—it either meets the criteria or it doesn't. No more ambiguous star ratings.

#### 2.3.4 The Living Gallery: Visible Impact
This is the core reward for the player: seeing their work change the world.
*   **Environmental Storytelling:** When an NPC accepts a photo, it doesn't just disappear. That photo is **physically placed into the game world.**
    *   Roric the Artisan will hang the photo on his workshop wall, and a new piece of pottery inspired by it may appear on his shelf a few days later.
    *   Linnea the Botanist will pin the macro shot to a specimen board in her greenhouse.
    *   Elara will place key discoveries on a large map in the central research tent, charting the expansion of the Guild's knowledge.
*   **The Guild Hall:** A central, initially empty building in the settlement acts as a community gallery. As major research assignments are completed, the player chooses their "masterpiece" photo from that assignment to be framed and hung in the Guild Hall. Over the course of the game, the player fills this hall with a visual history of their journey. This provides an immense sense of long-term accomplishment and personalization.

#### 2.3.5 The Role of the Codex
The Codex is no longer a progression tool, but it remains a vital personal tool.
*   **A Personal Field Journal:** It is the player's private, in-world photo album. Here, they can browse *all* their photos, not just the ones they bartered.
*   **Researcher's Notes:** The player can "favorite" photos and add their own notes, creating their own personal narrative of discovery.
*   **Information Hub:** When a photo is taken of a new creature or plant, the Codex auto-populates with basic, factual information. Fulfilling NPC requests may add more detailed, character-flavored lore entries to the relevant Codex page. For example, after giving Linnea a photo of a Moonpetal, she might add a note to the player's Codex about its medicinal properties.

## 3. World & Art Direction

### 3.1 Visual Identity
A highly stylized, painterly aesthetic blending the lush nature of **Studio Ghibli** with the ethereal wonder of **Journey** and **Gris**.
*   **Diegetic Design:** UI and game systems should feel like part of the world. The camera is a physical object; the Codex is a real book.
*   **Lighting as Character:** Light is not just for illumination; it is a physical, transformative force. God rays, caustics, and volumetric light are key visual features.

### 3.2 The Shifting Isles: World Structure
A series of colossal, earth-and-rock islands floating in a perpetual twilight sky.
*   **The Duality:** The world is dynamically altered by massive, slow-moving "rivers" of pure light and deep shadow that flow across the landscape. These are not just lighting effects; they are volumes that trigger state changes.

### 3.3 Dynamic Color Palettes
The world's color is not static but a direct result of the light's influence.
*   **Lumin Palette:** Warm golds (`#FFD700`), living greens (`#6B8E23`), dawn peaches (`#FFDAB9`). Feels like a perfect late-spring afternoon.
*   **Umbra Palette:** Deep indigos (`#483D8B`), amethyst glows (`#9932CC`), bioluminescent teals (`#20B2AA`). Feels like a magical forest at midnight.
*   **The Gloaming:** The transition zone where the two palettes blend, creating dusty roses, lavenders, and deep oranges. `Tech Note: Shaders will sample a global "Lightness" value to lerp between two color grading lookup tables (LUTs).`

### 3.4 Biomes & Environments
(See expanded table from previous version)

## 4. Creatures & AI System

### 4.1 AI Architecture Philosophy
Creatures must feel like wildlife, not scripted animatronics. The system is composed of three layers:
1.  **Sensor Component:** How the AI perceives the world. It detects player proximity, light levels, audible events (from the player), and locations of interest (food, water, dens).
2.  **State Machine:** The highest-level brain. The primary states are `Lumin` and `Umbra`. Other states include `Fleeing`, `Investigating`, `Sleeping`. Transitioning between Lumin/Umbra is the core mechanic.
3.  **Behavior Tree:** Governs actions *within* a state. A Behavior Tree for the `Lumin` state might include branches for `Wander`, `Graze`, and `Socialize`. The `Umbra` state would have a completely different tree. This allows for complex and varied behavior without rigid scripting.

### 4.2 MVP Creature Roster & Technicals

#### ### Creature 1: Flutterwing
*   **Concept:** A graceful insect/bird hybrid.
*   **Technical Notes:**
    *   **Animation:** Requires fast, looping wing flaps for Lumin form and slow, pulsing emissive animations for Umbra form. The transformation requires a complex blendshape/morph target animation.
    *   **AI:** Uses spline-based pathfinding to navigate between flower objects. Its 'curiosity' is low, 'skittishness' is moderate.
    *   **VFX:** Heavy use of trail renderers for wing tips and particle emitters for nectar/spore effects.

#### ### Creature 2: Grove-Strider
*   **Concept:** A majestic, shy, deer-like herbivore.
*   **Technical Notes:**
    *   **Animation:** Standard quadruped walk/run/idle cycles. The transformation is a shader effect combined with subtle animations, revealing emissive textures between its 'bark' plates.
    *   **AI:** Requires nav-mesh pathfinding. Has a large 'personal space' radius that triggers a `Flee` state. The flee behavior must be robust, finding a safe hiding spot in shadow or behind cover.
    *   **Shaders:** A key challenge is the Kintsugi-like reveal effect, likely driven by a noise texture and a dissolve factor uniform controlled by the AI state.

#### ### Creature 3: Skitter-Sprite
*   **Concept:** A small, curious, six-legged mammal.
*   **Technical Notes:**
    *   **Animation:** Fast, scurrying animations. Requires 'object interaction' animations for picking up and stashing shiny items.
    *   **AI:** High 'curiosity,' will approach the player if they remain still. AI needs to manage 'hoard' locations and remember them. The hunting behavior in Umbra form is a simple 'charge and pounce' routine.
    *   **Physics:** Its small size and fast movement make collision detection important. A capsule collider would be appropriate.

## 5. Audio Design

### 5.1 Sound Design Philosophy
Audio is a pillar of immersion. Every sound tells a story about the world.
*   **Ambient Soundscapes:** Rich, evolving layers of sound for each biome and light state (e.g., wind through crystalline trees vs. the low hum of glowing flora).
*   **Foley:** High-quality, satisfying sounds for player actions (footsteps on different surfaces, camera clicks, lens changes) and creature movements.

### 5.2 Dynamic Music System: Technicals
*   **Implementation:** The system constantly queries the world's 'light value' at the player's position (a 0.0-1.0 float). This can be sampled from a low-resolution lightmap or calculated based on proximity to light/shadow volumes.
*   **Crossfading:** This 0.0-1.0 value drives a linear interpolation between the volume of the Lumin track and the Umbra track. Both tracks are always loaded and playing silently, ensuring a seamless, zero-latency transition. The key and tempo match between tracks is critical for this to sound harmonious.

## 6. User Interface (UI/UX)

### 6.1 Design Philosophy: Diegetic & Minimal
The best UI is no UI. We avoid traditional video game HUD elements wherever possible to maximize immersion.
*   **No Persistent HUD:** In exploration mode, the screen is completely clear of overlays. Tool availability can be inferred by a subtle animation of the character's hands or belt.
*   **Contextual Prompts:** Interaction prompts appear as soft, fading text/glyphs near the object of interest, not in a fixed screen position.

### 6.2 Key UI Components
*   **Camera Viewfinder:** All photographic information (`f-stop`, `shutter speed`, `lens type`) is displayed as non-intrusive text overlays within the camera view, mimicking a real digital camera's display.
*   **The Codex:** A fully rendered 3D book in the Base Camp scene. The player uses mouse controls to turn pages. Left pages feature the best photo submitted; right pages contain graded stats, research notes, and lore.

## 7. Engine & Technical Specification

### 7.1 Core Architecture & Language
*   **Language:** C++17. Chosen for performance, control over memory, and robust ecosystem.
*   **Platform:** PC (Windows, Linux).
*   **Third-Party Libraries:**
    *   **GLFW:** Windowing and input context.
    *   **GLAD:** OpenGL function loading.
    *   **GLM:** Mathematics library for vector/matrix operations.
    *   **stb_image / stb_image_write:** Simple, header-only image loading/saving.
    *   **miniaudio:** Simple, header-only audio playback.
    *   **FastNoiseLite:** For performant procedural noise generation.

### 7.2 Rendering Pipeline Deep Dive
*   **Type:** Forward Rendering. Chosen for its simplicity and efficiency with multiple dynamic light sources (creature bioluminescence), which can be challenging for a simple Deferred Renderer.
*   **Render Passes (Order of Execution):**
    1.  **Shadow Pass:** Render scene from the directional light's perspective into a depth map.
    2.  **Z-Prepass:** Render all opaque geometry's depth to the main depth buffer. This reduces overdraw in the main pass.
    3.  **Opaque Pass:** Render all opaque and alpha-tested geometry (e.g., foliage) using the Z-Prepass depth buffer for early fragment rejection. Lighting (directional, point lights, shadows) is calculated here.
    4.  **Skybox Pass:** Render the skybox with depth testing enabled but depth writing disabled.
    5.  **Transparent Pass:** Render all transparent geometry (water, particles) back-to-front with depth writing disabled and blending enabled.
    6.  **Post-Processing Pass:** Apply full-screen effects like Depth of Field, Bloom, Color Grading (LUTs), and Vignetting by rendering a full-screen quad.

### 7.3 World Generation: "Echo LOD" System
*   **Rationale:** An infinite procedural world requires aggressive Level of Detail (LOD) management to maintain performance. This custom system is tailored to our blocky-but-smooth aesthetic.
*   **Meshing Algorithms:**
    *   **LOD 0 (Greedy Meshing):** Generates meshes with an extremely low vertex count for nearby terrain, ideal for blocky structures. `Trade-off: Can be complex to implement with varied block types and UVs.`
    *   **LOD 1 (Marching Cubes):** Generates a smoother, more organic mesh for mid-range terrain by sampling the noise field at a lower resolution. `Trade-off: Produces a high vertex count, but ideal for capturing the general shape.`
*   **LOD Transitions:** The seamless cross-fade is achieved via a dithered alpha pattern in the shader. `uniform float transitionFactor` controls the dither threshold. This is cheaper than true alpha blending and avoids sorting issues.
*   **Multithreading:** A dedicated pool of worker threads constantly runs meshing jobs. A thread-safe queue dispatches chunk coordinates to workers. Once a mesh is complete, its vertex data is pushed to a "ready queue" to be uploaded to the GPU on the main thread, preventing any stalls.

### 7.4 Physics & Player Controller
*   **Rationale:** The game does not require complex physics simulations. A full engine like Bullet or PhysX would be overkill. A simple, custom solution provides maximum control and performance.
*   **Implementation:** The player is represented by an Axis-Aligned Bounding Box (AABB). Each frame, movement velocity is applied, and the AABB is checked against the surrounding world mesh's triangles. A simple "slide" response is calculated upon collision. Gravity is a constant downward acceleration.

## 8. Development & Tooling

### 8.1 Source Control & Build System
*   **Source Control:** **Git**.
*   **Branching Model:** **GitFlow** (main, develop, feature/task-name, release, hotfix). This provides a structured workflow for a team.
*   **Build System:** **CMake**. Generates native build environments (Visual Studio solutions, Makefiles), making the project cross-platform and IDE-agnostic.

### 8.2 Code Architecture & Philosophy
*   **RAII (Resource Acquisition Is Initialization):** C++ best practices will be followed. Classes will manage their own memory and resources (e.g., a `Model` class loads its data in the constructor and frees it in the destructor).
*   **Data-Oriented Approach (for world gen):** The chunk data (blocks, light levels) will be stored in contiguous arrays for cache-friendly access by the meshing algorithms.
*   **SOLID Principles:** Code will be structured with Single Responsibility, Open/Closed principles in mind to promote modularity and extensibility.

### 8.3 Asset Pipeline & Tooling
*   **3D Modeling:** **Blender**. All models to be exported as `.fbx` with applied transforms.
*   **Texturing:** **Krita**, **Substance Painter**, or **Photoshop**. Textures to be exported as `.png`.
*   **Audio:** **Audacity** or **Reaper**. Sound effects as `.wav`, music as `.ogg`.
*   **Automation:** Scripts will be considered for batch processing of assets (e.g., texture compression).

### 8.4 In-Game Development Tools
A robust suite of debug tools is essential for efficiency. These are toggled via a debug console (`~` key).
*   **Debug Overlays:**
    *   **Wireframe Mode:** `r_wireframe 1`
    *   **AABB Visualization:** `debug_draw_aabb 1` (draws collision boxes for player and creatures)
    *   **AI State Display:** `ai_debug_state 1` (renders text above a creature's head showing its current state and behavior)
    *   **LOD Boundaries:** `r_show_lod_boundaries 1` (visualizes the different LOD radii around the player)
    *   **Profiler:** `prof_show 1` (displays simple frame time, CPU/GPU time, draw calls).
*   **Free-Cam:** A separate camera mode for developers to fly through the world unconstrained by physics.

## 9. Development Roadmap

### 9.1 Phase Breakdown
1.  **Phase 1: Engine Foundation:** Basic rendering, windowing, asset loading.
2.  **Phase 2: World Engine:** Implement the full "Echo LOD" system.
3.  **Phase 3: Gameplay Core:** Player controller, camera, and Codex systems.
4.  **Phase 4: The Living World:** Creature AI, asset integration, dynamic audio.

### 9.2 Minimum Viable Product (MVP) Definition
A polished, complete vertical slice of the experience.
*   **Engine:** Fully functional rendering pipeline with the complete "Echo LOD" system.
*   **Content:** One complete biome ("The Whispering Glade"), 3 fully realized creatures, Standard/Telephoto lenses.
*   **Gameplay:** A complete, functional loop from exploration to Codex grading.
*   **Audio:** Dynamic music system implemented for the MVP biome.

### 9.3 Post-Launch & Stretch Goals
*   Additional biomes, creatures, and lenses.
*   Advanced weather systems (glowing rain, shadow storms).
*   A dedicated Photo Mode with filters and frames.
*   Online gallery for players to share their best shots.

## 10. Document Control
*   **v1.0 (26-OCT-2023):** Initial creation of the GDD.
*   **v1.1 (26-OCT-2023):** Major expansion. Added deep technicals, rationale, and a full Developer & Tooling section.
*   **Status:** Concept & Prototyping Phase.
