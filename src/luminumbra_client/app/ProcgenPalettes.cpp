#include "app/ProcgenPalettes.h"

#include "core/Log.h"
#include "luminumbra_common/components/CombustionComponents.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/CreatureComponents.h"
#include "luminumbra_common/components/CropLifecycleComponents.h"
#include "luminumbra_common/components/DecayComponents.h"
#include "luminumbra_common/components/ForagingComponents.h"
#include "luminumbra_common/components/LightingComponents.h"
#include "luminumbra_common/core/DeterministicRng.h"
#include "luminumbra_common/foliage/SpeciesRegistry.h"
#include "luminumbra_common/systems/PlantGrowthSystem.h"
#include "luminumbra_common/systems/PlantProcgen.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "rendering/Mesh.h"
#include "rendering/RenderPipeline.h"

#include <algorithm>
#include <string>

namespace Luminumbra::Client::App {

// Re-bake the combined procgen plant mesh at growth `stageF` and push it to the pass. Young
// stages -> shallower branch recursion + smaller size; deterministic pure functions.
void BakeProcgenPlants(ProcgenPlantState& state,
                       Luminumbra::Rendering::PlantProcgenPass* pp,
                       float stageF) {
    if (!pp)
        return;
    if (state.plants.empty()) {
        pp->set_enabled(false);
        return;
    }
    const int kFruiting = static_cast<int>(Luminumbra::Components::PlantStage::Fruiting);
    const std::uint8_t stage =
        static_cast<std::uint8_t>(std::clamp(static_cast<int>(stageF), 0, kFruiting));
    const float growF =
        0.16f + 0.84f * std::clamp(stageF / static_cast<float>(kFruiting), 0.0f, 1.0f);
    luminumbra::foliage::PlantEnvDir env;
    env.sun_dir = state.sunDir;
    env.phototropism = 0.5f;
    std::vector<Luminumbra::Rendering::PlantProcgenPass::Vertex> verts;
    std::vector<std::uint32_t> indices;
    for (const ProcgenPlantInstance& inst : state.plants) {
        if (inst.suppressed)
            continue; // promoted to a sim PlantTag -> the sim tier draws it now
        const luminumbra::foliage::PlantStructure ps =
            luminumbra::foliage::GeneratePlant(inst.genome, stage, env);
        const luminumbra::foliage::ProcMesh pm = luminumbra::foliage::TessellatePlant(ps);
        const glm::mat3 rot = glm::mat3_cast(inst.rot);
        const float sc = inst.effScale * growF;
        const std::size_t leafVertStart = pm.vertices.size() >= ps.leaves.size() * 4u
                                              ? pm.vertices.size() - ps.leaves.size() * 4u
                                              : pm.vertices.size();
        // GENETIC + SEASONAL albedo: leaf greens vary by genome, shifting toward autumn
        // ochre with the season; bark brown varies subtly per genome.
        using G = Luminumbra::Components::PlantGene;
        const float hueVar = inst.genome.gene(G::LeafDensity);
        const float valVar = inst.genome.gene(G::Hardiness);
        const glm::vec3 summerLeaf(
            0.09f + 0.10f * hueVar, 0.32f + 0.22f * valVar, 0.07f + 0.05f * hueVar);
        const glm::vec3 autumnLeaf(0.42f + 0.10f * hueVar, 0.20f + 0.10f * valVar, 0.05f);
        const glm::vec3 leafColor =
            glm::mix(summerLeaf, autumnLeaf, std::clamp(state.season, 0.0f, 1.0f));
        const glm::vec3 barkColor(0.16f + 0.06f * valVar, 0.10f, 0.06f);
        const std::uint32_t baseVert = static_cast<std::uint32_t>(verts.size());
        for (std::size_t vi = 0; vi < pm.vertices.size(); ++vi) {
            const luminumbra::foliage::ProcVertex& src = pm.vertices[vi];
            Luminumbra::Rendering::PlantProcgenPass::Vertex v;
            v.pos = rot * (src.pos * sc) + inst.worldPos;
            v.normal = glm::normalize(rot * src.normal);
            const bool isLeaf = vi >= leafVertStart;
            v.uv = glm::vec2(isLeaf ? 1.0f : 0.0f, src.uv.y);
            v.color = isLeaf ? leafColor : barkColor;
            verts.push_back(v);
        }
        for (std::uint32_t idx : pm.indices)
            indices.push_back(baseVert + idx);
    }
    state.lastBakedStage = stageF;
    // Cache the tree mesh so the creature demo can composite creatures on top of it.
    state.treeVerts = verts;
    state.treeIndices = indices;
    if (verts.empty()) {
        pp->set_enabled(false);
        return;
    }
    const std::uint64_t sig = ((static_cast<std::uint64_t>(state.plants.size()) << 24) ^
                               static_cast<std::uint64_t>(stageF * 1000.0f)) |
                              1ull;
    pp->set_plants(verts, indices, sig);
    pp->set_enabled(true);
}

// Plant unification: composite the DECORATION-tier scatter (state.treeVerts cache, rebuilt by
// BakeProcgenPlants when the scatter changes) with the SIM-tier PlantTag plants (each at its live
// PlantGrowthComponent stage) into the single PlantProcgenPass. This is the ONE unified plant
// RENDER; the SIM stays split — only PlantTag entities tick/persist/hash, the vast scatter is
// render-only. So a player-planted/promoted plant ADDS to the forest rather than replacing it.
// Sig-gated on (scatter revision + the sim roster's ids/stages) so a settled frame is a no-op. Pure
// visual-only: reads sim truth, never writes back into the sim / world_hash.
std::size_t RebakeAllPlants(ProcgenPlantState& state,
                            Luminumbra::Rendering::PlantProcgenPass* pp,
                            const entt::registry& reg,
                            const glm::vec3& sunDir,
                            float season) {
    if (!pp)
        return 0;
    namespace C = Luminumbra::Components;
    auto view = reg.view<const C::PlantTag,
                         const C::PlantGenomeComponent,
                         const C::PlantGrowthComponent,
                         const C::TransformComponent>();
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end());

    // Cheap change key: scatter revision + each sim plant's (id, stage). Skip rebuild+upload when
    // unchanged (the common settled frame).
    std::uint64_t sig = 1469598103934665603ull ^ (state.scatterRevision * 1099511628211ull);
    for (const entt::entity e : ents) {
        const auto& gg = view.get<const C::PlantGrowthComponent>(e);
        sig = (sig ^ ((static_cast<std::uint64_t>(entt::to_integral(e)) << 8) ^
                      static_cast<std::uint64_t>(gg.stage))) *
              1099511628211ull;
    }
    sig |= 1ull;
    if (sig == state.lastCombinedPlantSig)
        return ents.size();
    state.lastCombinedPlantSig = sig;

    const int kFruiting = static_cast<int>(C::PlantStage::Fruiting);
    luminumbra::foliage::PlantEnvDir env;
    env.sun_dir = sunDir;
    env.phototropism = 0.5f;
    using G = C::PlantGene;

    // Base = the cached decoration scatter (already excludes promoted/suppressed instances).
    std::vector<Luminumbra::Rendering::PlantProcgenPass::Vertex> verts = state.treeVerts;
    std::vector<std::uint32_t> indices = state.treeIndices;
    for (const entt::entity e : ents) {
        const auto& genome = view.get<const C::PlantGenomeComponent>(e);
        const auto& g = view.get<const C::PlantGrowthComponent>(e);
        const auto& tf = view.get<const C::TransformComponent>(e);
        const std::uint8_t stage =
            static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(g.stage), 0, kFruiting));
        const float growF =
            0.16f + 0.84f * std::clamp(static_cast<float>(stage) / static_cast<float>(kFruiting),
                                       0.0f,
                                       1.0f);

        const luminumbra::foliage::PlantStructure ps =
            luminumbra::foliage::GeneratePlant(genome, stage, env);
        const luminumbra::foliage::ProcMesh pm = luminumbra::foliage::TessellatePlant(ps);

        // Deterministic per-plant yaw + scale from the entity id / genome (visual variety only).
        const std::uint32_t eid = static_cast<std::uint32_t>(entt::to_integral(e));
        const float yaw = (static_cast<float>((eid * 2654435761u) >> 8) / 16777216.0f) * 6.2831853f;
        const glm::mat3 rot = glm::mat3_cast(glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)));
        const float sc = (0.6f + 1.8f * genome.gene(G::MaxScale)) * growF;
        const glm::vec3 worldPos(tf.position.x, tf.position.y, tf.position.z);

        const std::size_t leafVertStart = pm.vertices.size() >= ps.leaves.size() * 4u
                                              ? pm.vertices.size() - ps.leaves.size() * 4u
                                              : pm.vertices.size();
        const float hueVar = genome.gene(G::LeafDensity);
        const float valVar = genome.gene(G::Hardiness);
        const glm::vec3 summerLeaf(
            0.09f + 0.10f * hueVar, 0.32f + 0.22f * valVar, 0.07f + 0.05f * hueVar);
        const glm::vec3 autumnLeaf(0.42f + 0.10f * hueVar, 0.20f + 0.10f * valVar, 0.05f);
        const glm::vec3 leafColor =
            glm::mix(summerLeaf, autumnLeaf, std::clamp(season, 0.0f, 1.0f));
        const glm::vec3 barkColor(0.16f + 0.06f * valVar, 0.10f, 0.06f);
        const std::uint32_t baseVert = static_cast<std::uint32_t>(verts.size());
        for (std::size_t vi = 0; vi < pm.vertices.size(); ++vi) {
            const luminumbra::foliage::ProcVertex& src = pm.vertices[vi];
            Luminumbra::Rendering::PlantProcgenPass::Vertex v;
            v.pos = rot * (src.pos * sc) + worldPos;
            v.normal = glm::normalize(rot * src.normal);
            const bool isLeaf = vi >= leafVertStart;
            v.uv = glm::vec2(isLeaf ? 1.0f : 0.0f, src.uv.y);
            v.color = isLeaf ? leafColor : barkColor;
            verts.push_back(v);
        }
        for (const std::uint32_t idx : pm.indices)
            indices.push_back(baseVert + idx);
    }
    if (verts.empty()) {
        pp->set_enabled(false);
        return ents.size();
    }
    pp->set_plants(verts, indices, sig);
    pp->set_enabled(true);
    return ents.size();
}

// Plant unification — PROMOTION: turn the decoration-tier scatter plant nearest `aim` (within
// `reach`) into a SIM-tier PlantTag entity, so the player can tend a wild forest tree into a
// living, growing, persisting plant. The promoted plant inherits the scatter instance's genome + a
// grown (Mature) perennial state; the scatter instance is SUPPRESSED (so it is not double-drawn)
// and the scatter revision bumped (the caller rebuilds the scatter cache). Sim stays bounded —
// promotion is gated by player interaction. Returns the new entity, or entt::null if no scatter
// plant is in reach.
entt::entity PromoteNearestScatter(ProcgenPlantState& state,
                                   entt::registry& reg,
                                   const glm::vec3& aim,
                                   float reach,
                                   std::uint64_t tick) {
    namespace C = Luminumbra::Components;
    int best = -1;
    float bestD = reach * reach;
    for (std::size_t i = 0; i < state.plants.size(); ++i) {
        const ProcgenPlantInstance& inst = state.plants[i];
        if (inst.suppressed)
            continue;
        const float dx = inst.worldPos.x - aim.x, dz = inst.worldPos.z - aim.z;
        const float d = dx * dx + dz * dz;
        if (d <= bestD) {
            bestD = d;
            best = static_cast<int>(i);
        }
    }
    if (best < 0)
        return entt::null;
    ProcgenPlantInstance& inst = state.plants[static_cast<std::size_t>(best)];
    const entt::entity e = reg.create();
    auto& tf = reg.emplace<C::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(inst.worldPos.x, inst.worldPos.y, inst.worldPos.z);
    reg.emplace<C::PlantTag>(e);
    reg.emplace<C::PlantGenomeComponent>(e, inst.genome);
    auto& g = reg.emplace<C::PlantGrowthComponent>(e);
    g.species_id = luminumbra::foliage::SpeciesId16("wild");
    g.stage = static_cast<std::uint8_t>(C::PlantStage::Mature); // a grown forest tree
    g.planted_tick = tick;
    g.last_tick = tick;
    auto& cl = reg.emplace<C::CropLifecycleComponent>(e); // promoted wild trees persist + regrow
    cl.perennial = true;
    cl.lifespan_ticks = 4800u;
    cl.species_id = g.species_id;
    inst.suppressed = true;
    ++state.scatterRevision;
    return e;
}

//  rebuild creature markers (small octahedra, red = predator, blue = prey) at the
// creatures' CURRENT positions and push to the procgen pass. Called per frame so the markers
// track the brain-driven movement. Render-only.
void BakeCreatureMarkers(Luminumbra::Rendering::PlantProcgenPass* pp,
                         entt::registry& reg,
                         Luminumbra::world::GameSession* gs) {
    if (!pp)
        return;
    // Markers only: the procedural FOREST now renders through the instanced static-mesh path
    // (vast, LOD'd), so this pass draws just the moving creature octahedra.
    // when `gs` is supplied, the forager colony (ants + food piles + nest anchor) is rendered
    // INTO THE SAME buffer so the shared PlantProcgenPass geometry is one upload (never two
    // set_plants calls clobbering each other).
    std::vector<Luminumbra::Rendering::PlantProcgenPass::Vertex> verts;
    std::vector<std::uint32_t> indices;
    auto view = reg.view<const Luminumbra::Components::CreatureComponent,
                         const Luminumbra::Components::TransformComponent>();
    constexpr float r = 1.0f, halfH = 1.3f; // ~matches the Jolt capsule; reads from the demo camera
    static const int tri[8][3] = {
        {0, 2, 3}, {0, 3, 4}, {0, 4, 5}, {0, 5, 2}, {1, 3, 2}, {1, 4, 3}, {1, 5, 4}, {1, 2, 5}};
    // Append one octahedron (centre c, horizontal radius rr, vertical half-height hh, colour).
    auto emitOcta = [&](const glm::vec3& c, float rr, float hh, const glm::vec3& color) {
        const glm::vec3 P[6] = {c + glm::vec3(0, hh, 0),
                                c - glm::vec3(0, hh, 0),
                                c + glm::vec3(rr, 0, 0),
                                c + glm::vec3(0, 0, rr),
                                c - glm::vec3(rr, 0, 0),
                                c - glm::vec3(0, 0, rr)};
        const std::uint32_t base = static_cast<std::uint32_t>(verts.size());
        for (const glm::vec3& p : P) {
            Luminumbra::Rendering::PlantProcgenPass::Vertex v;
            v.pos = p;
            v.normal = glm::normalize(p - c);
            v.uv = glm::vec2(0.0f, 0.0f); // bark flag -> no wind sway, vertex color albedo
            v.color = color;
            verts.push_back(v);
        }
        for (const auto& t : tri) {
            indices.push_back(base + t[0]);
            indices.push_back(base + t[1]);
            indices.push_back(base + t[2]);
        }
    };
    for (auto e : view) {
        const auto& tf = view.get<const Luminumbra::Components::TransformComponent>(e);
        const auto& cr = view.get<const Luminumbra::Components::CreatureComponent>(e);
        // The Jolt avatar body owns the position now (gravity/terrain collision), so the
        // transform's Y is the resolved capsule centre -- draw the marker right there.
        const glm::vec3 c(tf.position.x, tf.position.y, tf.position.z);
        // Prey are tinted by GENERATION (founders blue -> teal -> green -> lime) so new
        // generations born during the evolution demo are visually distinct; size follows the
        // heritable genome size_scale so trait drift shows too. Predator red, carcass bone.
        glm::vec3 preyCol(0.18f, 0.5f, 0.85f);
        float sizeMul = 1.0f;
        if (const auto* gn = reg.try_get<Luminumbra::Components::CreatureGenomeComponent>(e)) {
            static const glm::vec3 kGenPalette[4] = {{0.18f, 0.5f, 0.85f},
                                                     {0.18f, 0.82f, 0.72f},
                                                     {0.32f, 0.85f, 0.32f},
                                                     {0.75f, 0.85f, 0.2f}};
            preyCol = kGenPalette[gn->generation < 4u ? gn->generation : 3u];
            sizeMul = gn->size_scale;
        }
        glm::vec3 col = cr.eaten ? glm::vec3(0.92f, 0.88f, 0.75f) // dead -> pale bone carcass
                        : cr.is_predator ? glm::vec3(0.75f, 0.12f, 0.12f) // predator -> red
                                         : preyCol;                       // prey -> by generation
        // Decomposition: a decaying carcass SHRINKS + darkens to nothing (skip when fully gone),
        // so the population visibly self-bounds via death -> decay.
        if (const auto* dec = reg.try_get<Luminumbra::Components::DecayComponent>(e)) {
            if (dec->fully_decomposed)
                continue; // returned to the soil -> no marker
            if (dec->decay_ticks > 0 && dec->decay_duration > 0) {
                const float t =
                    static_cast<float>(dec->decay_ticks) / static_cast<float>(dec->decay_duration);
                sizeMul *= (1.0f - 0.8f * t); // shrink as it rots
                col *= (1.0f - 0.7f * t);     // darken toward the soil
            }
        }
        // rest poses: a resting/sleeping creature reads as "bedded
        // down" — the marker shrinks, SQUATS (flattens vertically), and dims, so a
        // sleeping subject is visibly distinct from an awake one. This is what a behaviour
        // photo objective ( BehavioralMatch) is shot against. Render-only; the
        // action is the brain's last_action (Rest=4, Sleep=5).
        float restSquash = 1.0f;
        if (cr.last_action == 5) { // Sleep — most settled
            sizeMul *= 0.6f;
            restSquash = 0.45f;
            col *= 0.70f;
        } else if (cr.last_action == 4) { // Rest — partly settled
            sizeMul *= 0.8f;
            restSquash = 0.70f;
            col *= 0.85f;
        }
        const float rr = r * sizeMul, hh = halfH * sizeMul * restSquash;
        emitOcta(c, rr, hh, col);
    }

    // forager colony render (anchor-only, client-visual). Renders the
    // ants shuttling, the food piles, and the nest ANCHOR. All reads (TransformComponent
    // mirror, cell->world, terrain height) are render-only; nothing steers or writes sim.
    if (gs) {
        auto* ws = gs->GetWorldSystem();
        int nestCx = -1, nestCz = -1;
        auto fgview = reg.view<const Luminumbra::Components::ForagerComponent,
                               const Luminumbra::Components::TransformComponent>();
        for (auto e : fgview) {
            const auto& fg = fgview.get<const Luminumbra::Components::ForagerComponent>(e);
            const auto& tf = fgview.get<const Luminumbra::Components::TransformComponent>(e);
            nestCx = fg.home_x;
            nestCz = fg.home_z; // every ant shares the nest cell
            const glm::vec3 c(tf.position.x, tf.position.y + 0.2f, tf.position.z);
            // A LADEN ant (carrying food home) glows amber; an outbound ant is pale.
            const glm::vec3 col =
                fg.carrying_food ? glm::vec3(0.95f, 0.65f, 0.15f) : glm::vec3(0.85f, 0.82f, 0.70f);
            emitOcta(c, 0.30f, 0.30f, col);
        }
        // Food piles (green), cell->world. Skip depleted sources.
        auto foodv = reg.view<const Luminumbra::Components::FoodSourceComponent>();
        for (auto e : foodv) {
            const auto& fs = foodv.get<const Luminumbra::Components::FoodSourceComponent>(e);
            if (fs.amount <= 0)
                continue;
            const float wx = gs->ScentCellToWorldX(fs.cell_x);
            const float wz = gs->ScentCellToWorldZ(fs.cell_z);
            const float wy = (ws ? ws->GetTerrainHeightAt(wx, wz) : 0.0f) + 0.4f;
            emitOcta(glm::vec3(wx, wy, wz), 0.7f, 0.7f, glm::vec3(0.30f, 0.80f, 0.25f));
        }
        // Nest ANCHOR (tan mound) at the colony's home cell — the "home" the trails radiate
        // from. Anchor-only: it is decoration, never a steering target (Option A).
        if (nestCx >= 0) {
            const float wx = gs->ScentCellToWorldX(nestCx);
            const float wz = gs->ScentCellToWorldZ(nestCz);
            const float wy = (ws ? ws->GetTerrainHeightAt(wx, wz) : 0.0f) + 0.5f;
            emitOcta(glm::vec3(wx, wy, wz), 1.2f, 0.9f, glm::vec3(0.55f, 0.40f, 0.25f));
        }

        // LUMIN CRYSTALS — a tall bright shard at each cave point-light. It sits
        // inside its own glow so it reads as a luminous crystal (and is the photo subject the
        // light makes visible). The PointLightComponent does the actual cave illumination.
        auto plview = reg.view<const Luminumbra::Components::PointLightComponent,
                               const Luminumbra::Components::TransformComponent>();
        for (auto e : plview) {
            const auto& pl = plview.get<const Luminumbra::Components::PointLightComponent>(e);
            const auto& tf = plview.get<const Luminumbra::Components::TransformComponent>(e);
            const glm::vec3 c(tf.position.x, tf.position.y, tf.position.z);
            const glm::vec3 col =
                glm::clamp(glm::vec3(pl.color.x, pl.color.y, pl.color.z) * 1.4f, 0.0f, 1.0f);
            emitOcta(c, 0.45f, 1.1f, col); // a slender upright crystal shard
        }
    }

    static std::uint64_t s_sig = 1000;
    ++s_sig; // creatures move every frame -> always re-upload
    if (verts.empty()) {
        pp->set_enabled(false);
        return;
    }
    pp->set_plants(verts, indices, s_sig);
    pp->set_enabled(true);
}

//  sim.fire demo: draw each combustible bush as an octahedron coloured by its DETERMINISTIC
// burn_state (green = unburnt, orange = burning, charcoal = burnt), grounded on the terrain.
// The FireSpreadSystem (now wired into the tick) drives the colours; this just visualizes them.
void BakeCombustibleMarkers(Luminumbra::Rendering::PlantProcgenPass* pp,
                            entt::registry& reg,
                            Luminumbra::Systems::SHIELD_WorldSystem* ws) {
    if (!pp)
        return;
    std::vector<Luminumbra::Rendering::PlantProcgenPass::Vertex> verts;
    std::vector<std::uint32_t> indices;
    auto view = reg.view<const Luminumbra::Components::CombustibleComponent,
                         const Luminumbra::Components::TransformComponent>();
    constexpr float r = 0.7f, halfH = 0.9f;
    static const int tri[8][3] = {
        {0, 2, 3}, {0, 3, 4}, {0, 4, 5}, {0, 5, 2}, {1, 3, 2}, {1, 4, 3}, {1, 5, 4}, {1, 2, 5}};
    for (auto e : view) {
        const auto& tf = view.get<const Luminumbra::Components::TransformComponent>(e);
        const auto& cb = view.get<const Luminumbra::Components::CombustibleComponent>(e);
        const float gy = ws ? ws->GetTerrainHeightAt(tf.position.x, tf.position.z) : tf.position.y;
        const glm::vec3 c(tf.position.x, gy + halfH, tf.position.z);
        glm::vec3 col(0.15f, 0.55f, 0.12f); // Unburnt -> green
        if (cb.state() == Luminumbra::Components::BurnState::Burning)
            col = glm::vec3(1.0f, 0.42f, 0.05f);
        else if (cb.state() == Luminumbra::Components::BurnState::Burnt)
            col = glm::vec3(0.09f, 0.08f, 0.07f);
        const glm::vec3 P[6] = {c + glm::vec3(0, halfH, 0),
                                c - glm::vec3(0, halfH, 0),
                                c + glm::vec3(r, 0, 0),
                                c + glm::vec3(0, 0, r),
                                c - glm::vec3(r, 0, 0),
                                c - glm::vec3(0, 0, r)};
        const std::uint32_t base = static_cast<std::uint32_t>(verts.size());
        for (const glm::vec3& p : P) {
            Luminumbra::Rendering::PlantProcgenPass::Vertex v;
            v.pos = p;
            v.normal = glm::normalize(p - c);
            v.uv = glm::vec2(0.0f, 0.0f);
            v.color = col;
            verts.push_back(v);
        }
        for (const auto& t : tri) {
            indices.push_back(base + t[0]);
            indices.push_back(base + t[1]);
            indices.push_back(base + t[2]);
        }
    }
    static std::uint64_t s_fsig = 5000;
    ++s_fsig;
    if (verts.empty()) {
        pp->set_enabled(false);
        return;
    }
    pp->set_plants(verts, indices, s_fsig);
    pp->set_enabled(true);
}

// VAST FOREST: a small PALETTE of procedurally-generated tree meshes (no baked model). Each
// palette entry is built once at world load into the instanced static-mesh cache (with 3 LODs,
// split into bark + leaf submeshes so the existing material LUT colours them), then the world
// scatters THOUSANDS of cheap instances of the palette through the engine's instanced + tree
// rendering LOD + frustum-cull path. Variety comes from the palette + per-instance transform; scale
// comes from instancing. The maintained path uses palette tinting and mesh LODs.
constexpr int kTreePaletteSize = 12; // distinct procedural trees (mix of species)

void BuildProcgenTreePalette(ProcgenPlantState& state,
                             Luminumbra::Rendering::RenderPipeline& rp,
                             const glm::vec3& sunDir) {
    if (state.treePaletteCount > 0)
        return; // build once
    namespace fol = luminumbra::foliage;
    fol::PlantEnvDir env;
    env.sun_dir = sunDir;
    env.phototropism = 0.5f;
    const std::uint8_t stage =
        static_cast<std::uint8_t>(Luminumbra::Components::PlantStage::Fruiting);
    const int radialForLod[3] = {8, 5, 3}; // branch detail per LOD (far = coarser)
    auto pal = luminumbra::core::DeterministicRng::seeded(fol::kPlantSeedOffset, 0xA11CE5ull, 99u);
    int built = 0;
    using V = Luminumbra::Rendering::Vertex;
    for (int p = 0; p < kTreePaletteSize; ++p) {
        const auto genome = fol::RandomGenome(pal);
        const fol::PlantStructure ps = fol::GeneratePlant(genome, stage, env);
        const std::size_t leafQuads = ps.leaves.size();
        glm::vec3 lo(1.0e9f), hi(-1.0e9f);         // tree AABB (from LOD0)
        float leafLoY = 1.0e9f, leafHiY = -1.0e9f; // canopy vertical band (leaf verts)
        for (int lod = 0; lod < 3; ++lod) {
            // Larger leaf cards -> fuller canopy from the same leaf COUNT (render-only
            // tessellation param; no PlantStructure/sim change, no world_hash impact).
            // The alpha-cut leaf texture overlaps into a denser canopy vs the default 0.20.
            const fol::ProcMesh pm = fol::TessellatePlant(ps, radialForLod[lod], 0.36f);
            const std::size_t leafStart = pm.vertices.size() >= leafQuads * 4u
                                              ? pm.vertices.size() - leafQuads * 4u
                                              : pm.vertices.size();
            std::vector<V> barkV, leafV;
            std::vector<std::uint32_t> barkI, leafI;
            barkV.reserve(leafStart);
            leafV.reserve(pm.vertices.size() - leafStart);
            for (std::size_t i = 0; i < pm.vertices.size(); ++i) {
                const fol::ProcVertex& s = pm.vertices[i];
                if (lod == 0) {
                    lo = glm::min(lo, s.pos);
                    hi = glm::max(hi, s.pos);
                    if (i >= leafStart) {
                        leafLoY = std::min(leafLoY, s.pos.y);
                        leafHiY = std::max(leafHiY, s.pos.y);
                    }
                }
                V v{s.pos, s.normal, s.uv};
                if (i < leafStart)
                    barkV.push_back(v);
                else
                    leafV.push_back(v);
            }
            for (std::uint32_t idx : pm.indices) {
                if (idx < leafStart)
                    barkI.push_back(idx);
                else
                    leafI.push_back(idx - static_cast<std::uint32_t>(leafStart));
            }
            const std::string base = "procgen://tree_" + std::to_string(p);
            const std::string suf = lod == 0 ? "" : (lod == 1 ? ".lod1" : ".lod2");
            rp.register_procgen_mesh(
                base + kBarkMatKey + suf,
                Luminumbra::Rendering::MeshLoader::CreateFromArrays(barkV, barkI));
            rp.register_procgen_mesh(
                base + kLeafMatKey + suf,
                Luminumbra::Rendering::MeshLoader::CreateFromArrays(leafV, leafI));
        }
        // LOD3 FAR-FIELD cross-billboard: a few quads spanning the tree's silhouette (~6 tris vs
        // hundreds), so a vast forest stays in budget out to the horizon. Leaf = two crossed
        // vertical quads over the canopy band; bark = one slim trunk quad. Sized from the LOD0
        // AABB.
        const float H = std::max(hi.y, 0.5f);
        const float W = std::max({hi.x, -lo.x, hi.z, -lo.z, 0.5f}); // canopy half-width
        const float cLo = (leafLoY < leafHiY) ? leafLoY : H * 0.35f;
        const float cHi = (leafHiY > leafLoY) ? leafHiY : H;
        auto addQuad = [](std::vector<V>& vv,
                          std::vector<std::uint32_t>& ii,
                          glm::vec3 a,
                          glm::vec3 b,
                          glm::vec3 c,
                          glm::vec3 d,
                          glm::vec3 n) {
            const std::uint32_t k = static_cast<std::uint32_t>(vv.size());
            vv.push_back({a, n, {0, 0}});
            vv.push_back({b, n, {1, 0}});
            vv.push_back({c, n, {1, 1}});
            vv.push_back({d, n, {0, 1}});
            ii.push_back(k);
            ii.push_back(k + 1);
            ii.push_back(k + 2);
            ii.push_back(k);
            ii.push_back(k + 2);
            ii.push_back(k + 3);
        };
        std::vector<V> bbLeafV;
        std::vector<std::uint32_t> bbLeafI;
        const glm::vec3 up(0, 1, 0);
        addQuad(
            bbLeafV, bbLeafI, {-W, cLo, 0}, {W, cLo, 0}, {W, cHi, 0}, {-W, cHi, 0}, up); // X-facing
        addQuad(
            bbLeafV, bbLeafI, {0, cLo, -W}, {0, cLo, W}, {0, cHi, W}, {0, cHi, -W}, up); // Z-facing
        // FAR-TREE LEAVES (rendering contract): the two crossed quads above are VERTICAL, so an
        // aerial / top-down view sees them edge-on and the canopy vanishes -> bare brown trunk
        // skeleton. Add a HORIZONTAL canopy "cap" over the top of the leaf band so looking DOWN
        // at a far tree still reads green leaf coverage. Up-facing normal (matches the leaf cards);
        // ~mid-to-upper canopy height; render-only, +2 tris/far tree, green tint via fs_in.Tint.
        const float capY = cLo + (cHi - cLo) * 0.72f;
        addQuad(bbLeafV,
                bbLeafI,
                {-W, capY, -W},
                {W, capY, -W},
                {W, capY, W},
                {-W, capY, W},
                up); // top cap
        std::vector<V> bbBarkV;
        std::vector<std::uint32_t> bbBarkI;
        const float tw = W * 0.12f;
        addQuad(bbBarkV,
                bbBarkI,
                {-tw, 0, 0},
                {tw, 0, 0},
                {tw, cLo, 0},
                {-tw, cLo, 0},
                glm::vec3(0, 0, 1));
        const std::string base = "procgen://tree_" + std::to_string(p);
        rp.register_procgen_mesh(
            base + kLeafMatKey + ".lod3",
            Luminumbra::Rendering::MeshLoader::CreateFromArrays(bbLeafV, bbLeafI));
        rp.register_procgen_mesh(
            base + kBarkMatKey + ".lod3",
            Luminumbra::Rendering::MeshLoader::CreateFromArrays(bbBarkV, bbBarkI));
        ++built;
    }
    state.treePaletteCount = built;
    LUMINUMBRA_CORE_INFO("VAST-FOREST: built procedural tree palette of {} entries (x4 LODs incl "
                         "far-field billboard)",
                         built);
}

// ROCK FORMATIONS (worldgen-richness ): a small palette of procedural
// faceted boulder meshes, registered into the instanced static-mesh cache and
// scattered as thousands of cheap instances (same render-only path as the trees;
// never hashed, no world_hash impact). Stone material id -> the stone triplanar
// texture via the instanced g_buffer path (no new art). Each palette entry is a
// deformed icosahedron (flat-shaded faces read as rocky), with per-entry
// non-uniform scale + per-vertex radial noise for variety.
constexpr int kRockPaletteSize = 8;
void BuildProcgenRockPalette(ProcgenPlantState& state, Luminumbra::Rendering::RenderPipeline& rp) {
    if (state.rockPaletteCount > 0)
        return;
    using V = Luminumbra::Rendering::Vertex;
    const float t = 1.6180339887f;
    const glm::vec3 ico[12] = {{-1, t, 0},
                               {1, t, 0},
                               {-1, -t, 0},
                               {1, -t, 0},
                               {0, -1, t},
                               {0, 1, t},
                               {0, -1, -t},
                               {0, 1, -t},
                               {t, 0, -1},
                               {t, 0, 1},
                               {-t, 0, -1},
                               {-t, 0, 1}};
    const int faces[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
                              {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                              {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
                              {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};
    auto h01 = [](int a, int b) {
        // Unsigned arithmetic throughout: `a * 73856093` as signed int overflows
        // (a can exceed 29 here) which is UB the -O3/LTO release build miscompiles
        // (debug wraps, release does not) — the root cause of a release-only hang
        // (degenerate face -> NaN normal -> stall). Unsigned wraps deterministically.
        std::uint32_t ua = static_cast<std::uint32_t>(static_cast<std::int64_t>(a)) * 73856093u;
        std::uint32_t ub = static_cast<std::uint32_t>(static_cast<std::int64_t>(b)) * 19349663u;
        std::uint64_t z = static_cast<std::uint64_t>(ua ^ ub) + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z = z ^ (z >> 31);
        return static_cast<float>((z >> 11) * (1.0 / 9007199254740992.0));
    };
    //   LOD: an octahedron hull (6 axis-extreme verts, 8 flat faces)
    // built from the deformed icosahedron's AABB — reads as the same boulder silhouette
    // at distance for ~8 tris (vs 20). Used for LOD1/LOD2 so distant rocks (>140m / >320m)
    // shed geometry; LOD3 (>620m) collapses to a crossed billboard (~4 tris).
    auto buildRockHull = [](const glm::vec3& lo, const glm::vec3& hi) {
        const glm::vec3 c = (lo + hi) * 0.5f;
        const glm::vec3 ex[6] = {{hi.x, c.y, c.z},
                                 {lo.x, c.y, c.z},
                                 {c.x, hi.y, c.z},
                                 {c.x, lo.y, c.z},
                                 {c.x, c.y, hi.z},
                                 {c.x, c.y, lo.z}};
        // 8 octahedron faces (+X/-X top/bottom rings).
        const int of[8][3] = {
            {0, 2, 4}, {4, 2, 1}, {1, 2, 5}, {5, 2, 0}, {0, 4, 3}, {4, 1, 3}, {1, 5, 3}, {5, 0, 3}};
        std::vector<V> v;
        std::vector<std::uint32_t> i;
        v.reserve(24);
        i.reserve(24);
        for (int f = 0; f < 8; ++f) {
            const glm::vec3 a = ex[of[f][0]], b = ex[of[f][1]], cc = ex[of[f][2]];
            const glm::vec3 cr = glm::cross(b - a, cc - a);
            const float crLen = glm::length(cr);
            const glm::vec3 nrm = (crLen > 1e-6f) ? (cr / crLen) : glm::vec3(0, 1, 0);
            const std::uint32_t k = static_cast<std::uint32_t>(v.size());
            v.push_back({a, nrm, {0, 0}});
            v.push_back({b, nrm, {1, 0}});
            v.push_back({cc, nrm, {0, 1}});
            i.push_back(k);
            i.push_back(k + 1);
            i.push_back(k + 2);
        }
        return Luminumbra::Rendering::MeshLoader::CreateFromArrays(v, i);
    };
    //  far-field billboard: two crossed vertical quads spanning the boulder AABB
    // (~4 tris) — the stone triplanar material colours them, so a distant scree field
    // stays in budget. Mirrors the tree LOD3 cross-billboard.
    auto buildRockBillboard = [](const glm::vec3& lo, const glm::vec3& hi) {
        const float W = std::max({hi.x, -lo.x, hi.z, -lo.z, 0.2f});
        const float yLo = lo.y, yHi = std::max(hi.y, lo.y + 0.2f);
        std::vector<V> v;
        std::vector<std::uint32_t> i;
        auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 n) {
            const std::uint32_t k = static_cast<std::uint32_t>(v.size());
            v.push_back({a, n, {0, 0}});
            v.push_back({b, n, {1, 0}});
            v.push_back({c, n, {1, 1}});
            v.push_back({d, n, {0, 1}});
            i.push_back(k);
            i.push_back(k + 1);
            i.push_back(k + 2);
            i.push_back(k);
            i.push_back(k + 2);
            i.push_back(k + 3);
        };
        quad({-W, yLo, 0}, {W, yLo, 0}, {W, yHi, 0}, {-W, yHi, 0}, {0, 0, 1});
        quad({0, yLo, -W}, {0, yLo, W}, {0, yHi, W}, {0, yHi, -W}, {1, 0, 0});
        return Luminumbra::Rendering::MeshLoader::CreateFromArrays(v, i);
    };
    int built = 0;
    for (int p = 0; p < kRockPaletteSize; ++p) {
        const glm::vec3 baseScale(
            0.7f + 0.7f * h01(p, 1), 0.45f + 0.7f * h01(p, 2), 0.7f + 0.7f * h01(p, 3));
        glm::vec3 dv[12];
        glm::vec3 lo(1.0e9f), hi(-1.0e9f);
        for (int i = 0; i < 12; ++i) {
            const glm::vec3 n = glm::normalize(ico[i]);
            const float r = 0.72f + 0.55f * h01(p * 13 + i, 7); // radial roughness
            dv[i] = n * r * baseScale;
            lo = glm::min(lo, dv[i]);
            hi = glm::max(hi, dv[i]);
        }
        std::vector<V> verts;
        std::vector<std::uint32_t> idx;
        verts.reserve(60);
        idx.reserve(60);
        for (int f = 0; f < 20; ++f) {
            const glm::vec3 a = dv[faces[f][0]], b = dv[faces[f][1]], c = dv[faces[f][2]];
            const glm::vec3 cr = glm::cross(b - a, c - a);
            const float crLen = glm::length(cr);
            const glm::vec3 nrm = (crLen > 1e-6f)
                                      ? (cr / crLen)       // flat-shaded face
                                      : glm::normalize(a); // degenerate -> radial fallback (no NaN)
            const std::uint32_t k = static_cast<std::uint32_t>(verts.size());
            verts.push_back({a, nrm, {0.0f, 0.0f}});
            verts.push_back({b, nrm, {1.0f, 0.0f}});
            verts.push_back({c, nrm, {0.0f, 1.0f}});
            idx.push_back(k);
            idx.push_back(k + 1);
            idx.push_back(k + 2);
        }
        const std::string base = "procgen://rock_" + std::to_string(p);
        rp.register_procgen_mesh(base,
                                 Luminumbra::Rendering::MeshLoader::CreateFromArrays(verts, idx));
        //  distance LODs (GBufferPass SelectTreeLod path picks these by camera
        // distance; absent = fall back to LOD0). LOD1+LOD2 = octahedron hull, LOD3 = billboard.
        rp.register_procgen_mesh(base + ".lod1", buildRockHull(lo, hi));
        rp.register_procgen_mesh(base + ".lod2", buildRockHull(lo, hi));
        rp.register_procgen_mesh(base + ".lod3", buildRockBillboard(lo, hi));
        ++built;
    }
    state.rockPaletteCount = built;
    LUMINUMBRA_CORE_INFO("VAST-FOREST: built procedural rock palette of {} entries", built);
}

// SHRUB/BUSH LAYER: a small palette of procedural bush meshes,
// registered into the SAME instanced static-mesh cache as the trees/rocks and
// scattered as cheap instances on flatter, vegetated ground (the opposite niche
// to the scree rocks). Each entry is a CLUSTER of 2-3 squashed, deformed
// icospheres (overlapping lobes read as a leafy shrub), green leaf material via
// the instanced g_buffer path (no new art).  (never hashed, no
// world_hash impact) — mirrors BuildProcgenRockPalette exactly, including the
// unsigned-only position hash (signed int*prime overflow is release-only UB that
// miscompiles to a NaN-normal stall — see memory procgen-hash-signed-overflow-ub).
constexpr int kBushPaletteSize = 6;
void BuildProcgenBushPalette(ProcgenPlantState& state, Luminumbra::Rendering::RenderPipeline& rp) {
    if (state.bushPaletteCount > 0)
        return;
    using V = Luminumbra::Rendering::Vertex;
    const float t = 1.6180339887f;
    const glm::vec3 ico[12] = {{-1, t, 0},
                               {1, t, 0},
                               {-1, -t, 0},
                               {1, -t, 0},
                               {0, -1, t},
                               {0, 1, t},
                               {0, -1, -t},
                               {0, 1, -t},
                               {t, 0, -1},
                               {t, 0, 1},
                               {-t, 0, -1},
                               {-t, 0, 1}};
    const int faces[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
                              {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                              {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
                              {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};
    auto h01 = [](int a, int b) {
        std::uint32_t ua = static_cast<std::uint32_t>(static_cast<std::int64_t>(a)) * 73856093u;
        std::uint32_t ub = static_cast<std::uint32_t>(static_cast<std::int64_t>(b)) * 19349663u;
        std::uint64_t z = static_cast<std::uint64_t>(ua ^ ub) + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z = z ^ (z >> 31);
        return static_cast<float>((z >> 11) * (1.0 / 9007199254740992.0));
    };
    //   LOD helpers (mirror the rock palette): an 8-face octahedron mound
    // sized to the cluster AABB (~8 tris vs 40-60) for LOD1/LOD2, and a crossed billboard
    // (~4 tris) for the far field. The grass/leaf material colours them, so distant
    // undergrowth stays in budget. Selected by GBufferPass SelectTreeLod by camera distance.
    auto buildBushHull = [](const glm::vec3& lo, const glm::vec3& hi) {
        const glm::vec3 c = (lo + hi) * 0.5f;
        const glm::vec3 ex[6] = {{hi.x, c.y, c.z},
                                 {lo.x, c.y, c.z},
                                 {c.x, hi.y, c.z},
                                 {c.x, lo.y, c.z},
                                 {c.x, c.y, hi.z},
                                 {c.x, c.y, lo.z}};
        const int of[8][3] = {
            {0, 2, 4}, {4, 2, 1}, {1, 2, 5}, {5, 2, 0}, {0, 4, 3}, {4, 1, 3}, {1, 5, 3}, {5, 0, 3}};
        std::vector<V> v;
        std::vector<std::uint32_t> i;
        v.reserve(24);
        i.reserve(24);
        for (int f = 0; f < 8; ++f) {
            const glm::vec3 a = ex[of[f][0]], b = ex[of[f][1]], cc = ex[of[f][2]];
            const glm::vec3 cr = glm::cross(b - a, cc - a);
            const float crLen = glm::length(cr);
            const glm::vec3 nrm = (crLen > 1e-6f) ? (cr / crLen) : glm::vec3(0, 1, 0);
            const std::uint32_t k = static_cast<std::uint32_t>(v.size());
            v.push_back({a, nrm, {0, 0}});
            v.push_back({b, nrm, {1, 0}});
            v.push_back({cc, nrm, {0, 1}});
            i.push_back(k);
            i.push_back(k + 1);
            i.push_back(k + 2);
        }
        return Luminumbra::Rendering::MeshLoader::CreateFromArrays(v, i);
    };
    auto buildBushBillboard = [](const glm::vec3& lo, const glm::vec3& hi) {
        const float W = std::max({hi.x, -lo.x, hi.z, -lo.z, 0.2f});
        const float yLo = std::min(lo.y, 0.0f), yHi = std::max(hi.y, yLo + 0.2f);
        std::vector<V> v;
        std::vector<std::uint32_t> i;
        auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 n) {
            const std::uint32_t k = static_cast<std::uint32_t>(v.size());
            v.push_back({a, n, {0, 0}});
            v.push_back({b, n, {1, 0}});
            v.push_back({c, n, {1, 1}});
            v.push_back({d, n, {0, 1}});
            i.push_back(k);
            i.push_back(k + 1);
            i.push_back(k + 2);
            i.push_back(k);
            i.push_back(k + 2);
            i.push_back(k + 3);
        };
        quad({-W, yLo, 0}, {W, yLo, 0}, {W, yHi, 0}, {-W, yHi, 0}, {0, 0, 1});
        quad({0, yLo, -W}, {0, yLo, W}, {0, yHi, W}, {0, yHi, -W}, {1, 0, 0});
        return Luminumbra::Rendering::MeshLoader::CreateFromArrays(v, i);
    };
    int built = 0;
    for (int p = 0; p < kBushPaletteSize; ++p) {
        std::vector<V> verts;
        std::vector<std::uint32_t> idx;
        verts.reserve(180);
        idx.reserve(180);
        glm::vec3 lo(1.0e9f), hi(-1.0e9f); // cluster AABB for the LOD hull/billboard
        // 2-3 overlapping lobes per bush; each a squashed, deformed icosphere.
        const int lobes = 2 + static_cast<int>(h01(p, 41) * 2.0f); // 2..3
        for (int l = 0; l < lobes; ++l) {
            // lobe centre offset (low + spread out so the cluster reads as a mound).
            const glm::vec3 centre((h01(p * 7 + l, 11) - 0.5f) * 0.9f,
                                   0.18f + 0.30f * h01(p * 7 + l, 12),
                                   (h01(p * 7 + l, 13) - 0.5f) * 0.9f);
            // squashed (wider than tall) so it sits like a shrub, not a ball.
            const glm::vec3 lobeScale(0.40f + 0.30f * h01(p * 7 + l, 14),
                                      0.26f + 0.22f * h01(p * 7 + l, 15),
                                      0.40f + 0.30f * h01(p * 7 + l, 16));
            glm::vec3 dv[12];
            for (int i = 0; i < 12; ++i) {
                const glm::vec3 n = glm::normalize(ico[i]);
                const float r = 0.78f + 0.42f * h01((p * 7 + l) * 13 + i, 7); // leafy roughness
                dv[i] = centre + n * r * lobeScale;
                lo = glm::min(lo, dv[i]);
                hi = glm::max(hi, dv[i]);
            }
            for (int f = 0; f < 20; ++f) {
                const glm::vec3 a = dv[faces[f][0]], b = dv[faces[f][1]], c = dv[faces[f][2]];
                const glm::vec3 cr = glm::cross(b - a, c - a);
                const float crLen = glm::length(cr);
                const glm::vec3 nrm =
                    (crLen > 1e-6f) ? (cr / crLen)
                                    : glm::normalize(a - centre + glm::vec3(0.0f, 1e-3f, 0.0f));
                const std::uint32_t k = static_cast<std::uint32_t>(verts.size());
                verts.push_back({a, nrm, {0.0f, 0.0f}});
                verts.push_back({b, nrm, {1.0f, 0.0f}});
                verts.push_back({c, nrm, {0.0f, 1.0f}});
                idx.push_back(k);
                idx.push_back(k + 1);
                idx.push_back(k + 2);
            }
        }
        const std::string base = "procgen://bush_" + std::to_string(p);
        rp.register_procgen_mesh(base,
                                 Luminumbra::Rendering::MeshLoader::CreateFromArrays(verts, idx));
        //  distance LODs (GBufferPass picks by camera distance; absent -> LOD0).
        rp.register_procgen_mesh(base + ".lod1", buildBushHull(lo, hi));
        rp.register_procgen_mesh(base + ".lod2", buildBushHull(lo, hi));
        rp.register_procgen_mesh(base + ".lod3", buildBushBillboard(lo, hi));
        ++built;
    }
    state.bushPaletteCount = built;
    LUMINUMBRA_CORE_INFO("VAST-FOREST: built procedural bush palette of {} entries", built);
}

} // namespace Luminumbra::Client::App
