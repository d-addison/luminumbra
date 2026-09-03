#pragma once

// Procedural plant/rock/bush render content for the client app, extracted
// verbatim from main_client.cpp: the decoration-tier procgen plant scatter
// state, the combined plant bake / re-bake / promotion helpers, the creature
// and combustible marker bakes (drawn through the same PlantProcgenPass), and
// the instanced tree/rock/bush palette builders. Everything here is
// render-only decoration — never sim state, never hashed.

#include "luminumbra_common/components/PlantComponents.h"
#include "rendering/passes/PlantProcgenPass.h"

#include "entt/entt.hpp"

#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

namespace Luminumbra::Rendering {
class RenderPipeline;
}
namespace Luminumbra::world {
class GameSession;
}
namespace Luminumbra::Systems {
class SHIELD_WorldSystem;
}

namespace Luminumbra::Client::App {

//  stored procgen plant instances so the geometry can be RE-BAKED at a changing
// growth stage (the live-growth render bridge) -- a plant grows sapling->tree over time.
struct ProcgenPlantInstance {
    glm::vec3 worldPos;
    glm::quat rot;
    float effScale;
    Luminumbra::Components::PlantGenomeComponent genome;
    // Plant unification: a decoration-tier scatter plant PROMOTED to a sim PlantTag entity (on
    // player interaction) is suppressed here so it is not double-drawn — the sim tier now owns it.
    bool suppressed = false;
};

// Bundled procgen plant/scatter render state — formerly a set of main_client.cpp
// globals. main_client.cpp owns the single instance and passes it by reference.
struct ProcgenPlantState {
    std::vector<ProcgenPlantInstance> plants;
    // Bumped whenever the scatter set changes (a promotion suppresses an instance) so the combined
    // plant re-bake knows to rebuild the cached scatter geometry. The sim tier is keyed separately.
    std::uint64_t scatterRevision = 0;
    std::uint64_t lastCombinedPlantSig = 0; // cache key for the composited scatter+sim mesh upload
    // Cache of the last-baked procedural TREE mesh, so the creature timelapse can draw the
    // programmatic trees AND the moving creature markers through the single PlantProcgenPass
    // (creatures are appended to this each frame).
    std::vector<Luminumbra::Rendering::PlantProcgenPass::Vertex> treeVerts;
    std::vector<std::uint32_t> treeIndices;
    float stageF = 5.0f; // growth: 0 = Seed.. 5 = Fruiting (drives structure + size)
    float lastBakedStage = -2.0f;
    glm::vec3 sunDir = glm::vec3(0.0f, 1.0f, 0.0f);
    float season = 0.0f; // 0 = summer green.. 1 = autumn ochre (seasonal leaf color)
    // Instanced palette entry counts (each palette builds once per world load; 0 = not built).
    int treePaletteCount = 0;
    int rockPaletteCount = 0;
    int bushPaletteCount = 0;
};

// Instanced-mesh material-key suffixes the palette builders register bark/leaf
// submeshes under (the scatter consume code appends them to "procgen://tree_N").
inline const char* const kBarkMatKey = "_bark";
inline const char* const kLeafMatKey = "_leaf";

// Re-bake the combined procgen plant mesh at growth `stageF` and push it to the pass.
void BakeProcgenPlants(ProcgenPlantState& state,
                       Luminumbra::Rendering::PlantProcgenPass* pp,
                       float stageF);

// Composite the decoration-tier scatter with the sim-tier PlantTag plants into
// the single PlantProcgenPass upload. Returns the sim plant count.
std::size_t RebakeAllPlants(ProcgenPlantState& state,
                            Luminumbra::Rendering::PlantProcgenPass* pp,
                            const entt::registry& reg,
                            const glm::vec3& sunDir,
                            float season);

// Promote the decoration-tier scatter plant nearest `aim` (within `reach`) into
// a sim-tier PlantTag entity. Returns the new entity, or entt::null.
entt::entity PromoteNearestScatter(ProcgenPlantState& state,
                                   entt::registry& reg,
                                   const glm::vec3& aim,
                                   float reach,
                                   std::uint64_t tick);

// Rebuild the creature markers (and, with `gs`, the forager colony + lumin
// crystals) at the creatures' current positions and push them to the pass.
void BakeCreatureMarkers(Luminumbra::Rendering::PlantProcgenPass* pp,
                         entt::registry& reg,
                         Luminumbra::world::GameSession* gs = nullptr);

// Draw each combustible bush as an octahedron coloured by its burn_state.
void BakeCombustibleMarkers(Luminumbra::Rendering::PlantProcgenPass* pp,
                            entt::registry& reg,
                            Luminumbra::Systems::SHIELD_WorldSystem* ws);

// Build the procedural tree / rock / bush palettes into the instanced
// static-mesh cache (each builds once; the counts land in `state`).
void BuildProcgenTreePalette(ProcgenPlantState& state,
                             Luminumbra::Rendering::RenderPipeline& rp,
                             const glm::vec3& sunDir);
void BuildProcgenRockPalette(ProcgenPlantState& state, Luminumbra::Rendering::RenderPipeline& rp);
void BuildProcgenBushPalette(ProcgenPlantState& state, Luminumbra::Rendering::RenderPipeline& rp);

} // namespace Luminumbra::Client::App
