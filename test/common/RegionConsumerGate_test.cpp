#include "../support/UniqueTempPath.h"
#include "luminumbra_common/ai/CreatureBrainSystem.h"
#include "luminumbra_common/ai/CreatureReproductionSystem.h"
#include "luminumbra_common/ai/LifespanSystem.h"
#include "luminumbra_common/ai/PerceptionSystem.h"
#include "luminumbra_common/ai/WildlifeFoliageSystem.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/persistence/PlantPersistence.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/CropLifecycleSystem.h"
#include "luminumbra_common/systems/FireSpreadSystem.h"
#include "luminumbra_common/systems/PlantCoarse.h"
#include "luminumbra_common/systems/PlantDiseaseSystem.h"
#include "luminumbra_common/systems/PollinationSystem.h"
#include "luminumbra_common/systems/SoilNutrientSystem.h"
#include "luminumbra_common/world/GameSession.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// Independent full-rate reference for movement, needs, courtship and seeded births.
namespace LegacyCreatureTickOracle {
using namespace luminumbra::ai;
namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

inline CreatureBrainStats
RunCreatureBrainSystemOnTick(entt::registry& reg,
                             float dt,
                             const EcologyTuning& tuning = {},
                             const ScentField* scent = nullptr,
                             float scent_origin_x = 0.0f,
                             float scent_origin_z = 0.0f,
                             float scent_cell_size = 0.0f,
                             bool use_perception_substrate = true,
                             const CreatureSpeciesRegistry* species = nullptr) {
    CreatureBrainStats stats;
    auto view = reg.view<Comp::CreatureComponent, Comp::TransformComponent>();
    std::vector<entt::entity> ents;
    for (auto e : view)
        ents.push_back(e);
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    struct Snap {
        entt::entity e;
        float x, z;
        float hx, hz;
        bool predator;
        bool eaten;
    };
    std::vector<Snap> snap;
    snap.reserve(ents.size());
    for (auto e : ents) {
        const auto& tf = view.get<Comp::TransformComponent>(e);
        const auto& c = view.get<Comp::CreatureComponent>(e);
        snap.push_back(
            {e, tf.position.x, tf.position.z, c.wish_x, c.wish_z, c.is_predator, c.eaten});
    }
    UniformSpatialGrid predGrid(tuning.flock_neighbor_radius);
    UniformSpatialGrid preyGrid(tuning.flock_neighbor_radius);
    {
        std::vector<GridPoint> predPts, preyPts;
        predPts.reserve(snap.size());
        preyPts.reserve(snap.size());
        for (std::uint32_t i = 0; i < snap.size(); ++i) {
            const Snap& o = snap[i];
            (o.predator ? predPts : preyPts).push_back({i, o.x, o.z});
        }
        predGrid.Build(predPts);
        preyGrid.Build(preyPts);
    }
    std::vector<std::uint32_t> herdHits;
    PerceptionField predTargetField;
    PerceptionField preyTargetField;
    PerceptionSnapshot targetSnapshot;
    if (use_perception_substrate) {
        std::vector<PerceptionSourceInput> predSources, preySources;
        predSources.reserve(snap.size());
        preySources.reserve(snap.size());
        for (std::uint32_t i = 0; i < snap.size(); ++i) {
            const Snap& o = snap[i];
            if (o.eaten)
                continue;
            PerceptionSourceInput in;
            in.index = i;
            in.has_position = true;
            in.x = o.x;
            in.z = o.z;
            in.radius = 0.0f;
            (o.predator ? predSources : preySources).push_back(in);
        }
        predTargetField.Build(predSources);
        preyTargetField.Build(preySources);
    }
    for (std::size_t selfIdx = 0; selfIdx < ents.size(); ++selfIdx) {
        const entt::entity e = ents[selfIdx];
        auto& tf = view.get<Comp::TransformComponent>(e);
        auto& cr = view.get<Comp::CreatureComponent>(e);
        if (cr.eaten) {
            cr.wish_x = 0.0f;
            cr.wish_z = 0.0f;
            ++stats.updated;
            continue;
        }
        const float sx = tf.position.x, sz = tf.position.z;
        const Comp::CreatureGenomeComponent* sg = reg.try_get<Comp::CreatureGenomeComponent>(e);
        float faceX = 0.0f, faceZ = 0.0f;
        if (sg != nullptr) {
            const float hh =
                dm::Sqrt(snap[selfIdx].hx * snap[selfIdx].hx + snap[selfIdx].hz * snap[selfIdx].hz);
            if (hh > 1e-4f) {
                faceX = snap[selfIdx].hx / hh;
                faceZ = snap[selfIdx].hz / hh;
            }
        }
        const float starve01 = utility_clamp01((cr.hunger - 0.85f) / 0.15f);
        const float starve_degrade = 1.0f - 0.5f * starve01;
        float bestDist = 1.0e9f, tx = sx, tz = sz;
        bool found = false;
        entt::entity te = entt::null;
        auto considerTarget = [&](const Snap& o) {
            const float dx = o.x - sx, dz = o.z - sz;
            const float d = dm::Sqrt(dx * dx + dz * dz);
            if (sg != nullptr) {
                bool sensed = (d <= sg->hearing_range * starve_degrade);
                if (!sensed && d <= sg->vision_range * starve_degrade) {
                    if (faceX == 0.0f && faceZ == 0.0f) {
                        sensed = true;
                    } else if (d > 1e-4f) {
                        sensed = ((dx * faceX + dz * faceZ) / d) >= sg->vision_cos_half_fov;
                    } else {
                        sensed = true;
                    }
                }
                if (!sensed)
                    return;
            }
            if (d < bestDist) {
                bestDist = d;
                tx = o.x;
                tz = o.z;
                te = o.e;
                found = true;
            }
        };
        if (use_perception_substrate) {
            const PerceptionField& targetField = cr.is_predator ? preyTargetField : predTargetField;
            PerceptionQueryInput tq;
            tq.has_position = true;
            tq.x = sx;
            tq.z = sz;
            targetField.Query(tq, targetSnapshot);
            for (const PerceivedSource& ps : targetSnapshot.perceived) {
                considerTarget(snap[ps.index]);
            }
        } else {
            for (const Snap& o : snap) {
                if (o.e == e || o.predator == cr.is_predator || o.eaten)
                    continue;
                considerTarget(o);
            }
        }
        if (scent != nullptr && cr.is_predator && !found && scent_cell_size > 0.0f) {
            const int cx = WorldToScentCell(sx, scent_origin_x, scent_cell_size);
            const int cz = WorldToScentCell(sz, scent_origin_z, scent_cell_size);
            float gdx = 0.0f, gdz = 0.0f;
            const float conf = scent->SimulationCellAllowed(cx, cz)
                                   ? scent->GradientSteer(/*ch=*/0,
                                                          cx,
                                                          cz,
                                                          /*sign=*/+1.0f,
                                                          /*floor=*/1e-4f,
                                                          /*k=*/0.05f,
                                                          gdx,
                                                          gdz)
                                   : 0.0f;
            if (conf > 0.0f) {
                tx = sx + gdx * 12.0f;
                tz = sz + gdz * 12.0f;
                bestDist = 20.0f;
                found = true;
            }
        }
        if (cr.is_predator && found && bestDist < tuning.catch_radius && reg.valid(te) &&
            !reg.get<Comp::CreatureComponent>(te).eaten) {
            reg.get<Comp::CreatureComponent>(te).eaten = true;
            cr.hunger = utility_clamp01(cr.hunger - tuning.catch_satiation);
        }
        CreatureSenses s;
        s.is_predator = cr.is_predator;
        s.hunger = cr.hunger;
        s.stamina = cr.stamina;
        s.energy = cr.energy;
        if (const auto* cc = reg.try_get<Comp::CircadianComponent>(e))
            s.circadian_activity = cc->activity;
        const auto* th = reg.try_get<Comp::ThirstComponent>(e);
        if (th != nullptr) {
            s.thirst = th->thirst;
            s.water_proximity = th->water_proximity;
        }
        const float nearNorm = found ? (1.0f - utility_clamp01(bestDist / 30.0f)) : 0.0f;
        if (cr.is_predator) {
            s.food_proximity = nearNorm;
        } else {
            s.threat_proximity = nearNorm;
            s.food_proximity = 0.6f;
            if (auto* al = reg.try_get<Comp::AlarmComponent>(e)) {
                al->alarmed = (nearNorm > 0.4f) ? 1u : 0u;
                if (al->level > s.threat_proximity)
                    s.threat_proximity = al->level;
            }
        }
        const CreatureSpecies* species_definition =
            species ? species->Find(cr.species_id) : nullptr;
        const CreatureAction act = DecideCreatureAction(
            s, species_definition ? species_definition->brain : CreatureBrainParams{});
        cr.last_action = static_cast<int>(act);
        float dirx = 0.0f, dirz = 0.0f;
        switch (act) {
            case CreatureAction::Flee:
                if (found) {
                    dirx = sx - tx;
                    dirz = sz - tz;
                }
                break;
            case CreatureAction::Hunt:
                if (found) {
                    dirx = tx - sx;
                    dirz = tz - sz;
                }
                break;
            case CreatureAction::Wander: {
                const float ang = 2.39996323f * static_cast<float>(entt::to_integral(e) % 16u + 1u);
                dirx = dm::Cos(ang);
                dirz = dm::Sin(ang);
                break;
            }
            case CreatureAction::Graze:
                cr.hunger = utility_clamp01(cr.hunger - tuning.hunger_graze_sate * dt);
                break;
            case CreatureAction::Rest:
                cr.stamina = utility_clamp01(cr.stamina + tuning.stamina_rest_recover * dt);
                cr.energy = utility_clamp01(cr.energy + tuning.energy_rest_recover * dt);
                break;
            case CreatureAction::Sleep:
                cr.stamina = utility_clamp01(cr.stamina + tuning.stamina_rest_recover * dt);
                cr.energy = utility_clamp01(cr.energy + tuning.energy_sleep_recover * dt);
                break;
            case CreatureAction::Drink:
                if (th != nullptr) {
                    dirx = th->wish_x;
                    dirz = th->wish_z;
                }
                break;
        }
        float adirx = 0.0f, adirz = 0.0f;
        const float alen = dm::Sqrt(dirx * dirx + dirz * dirz);
        if (alen > 1.0e-5f) {
            adirx = dirx / alen;
            adirz = dirz / alen;
        }
        if (act == CreatureAction::Flee || act == CreatureAction::Hunt ||
            act == CreatureAction::Wander) {
            herdHits.clear();
            const UniformSpatialGrid& grid = cr.is_predator ? predGrid : preyGrid;
            grid.QueryRadius(sx, sz, herdHits);
            std::vector<std::pair<float, float>> herd;
            std::vector<std::pair<float, float>> herdHeadings;
            herd.reserve(herdHits.size());
            herdHeadings.reserve(herdHits.size());
            for (std::uint32_t hi : herdHits) {
                if (static_cast<std::size_t>(hi) == selfIdx)
                    continue;
                herd.emplace_back(snap[hi].x, snap[hi].z);
                herdHeadings.emplace_back(snap[hi].hx, snap[hi].hz);
            }
            FlockParams fp{};
            fp.neighbor_radius = tuning.flock_neighbor_radius;
            fp.separation_radius = tuning.flock_separation_radius;
            fp.cohesion_weight = tuning.flock_cohesion_weight;
            fp.separation_weight = tuning.flock_separation_weight;
            fp.alignment_weight = tuning.alignment_weight;
            const FlockSteer fs = ComputeFlockSteer(sx, sz, herd, fp, &herdHeadings);
            adirx += fs.x * tuning.herd_weight;
            adirz += fs.z * tuning.herd_weight;
        }
        const float len = dm::Sqrt(adirx * adirx + adirz * adirz);
        cr.wish_x = 0.0f;
        cr.wish_z = 0.0f;
        if (len > 1.0e-5f) {
            const float inv = 1.0f / len;
            const float speed =
                ((act == CreatureAction::Flee || act == CreatureAction::Hunt) ? cr.move_speed * 1.5f
                                                                              : cr.move_speed) *
                starve_degrade;
            cr.wish_x = adirx * inv * speed;
            cr.wish_z = adirz * inv * speed;
            cr.stamina = utility_clamp01(cr.stamina - tuning.stamina_move_drain * dt);
            if (!reg.all_of<Comp::CreaturePhysicsComponent>(e)) {
                tf.position.x += cr.wish_x * dt;
                tf.position.z += cr.wish_z * dt;
            }
        }
        cr.hunger = utility_clamp01(cr.hunger + tuning.hunger_growth_per_second * dt);
        cr.energy = utility_clamp01(cr.energy - tuning.energy_drain_per_second * dt);
        ++stats.updated;
    }
    return stats;
}

inline void RunMateSeekingOnTick(entt::registry& reg, const ReproductionTuning& tuning = {}) {
    auto view = reg.view<Comp::CreatureComponent,
                         Comp::CreatureGenomeComponent,
                         Comp::TransformComponent>();
    struct M {
        entt::entity e;
        float x, z;
        bool female;
        bool ready;
        std::uint16_t species_id;
    };
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    std::vector<M> snap;
    snap.reserve(ents.size());
    for (auto e : ents) {
        const auto& tf = view.get<Comp::TransformComponent>(e);
        const auto& cr = view.get<Comp::CreatureComponent>(e);
        const auto& gn = view.get<Comp::CreatureGenomeComponent>(e);
        snap.push_back({e,
                        tf.position.x,
                        tf.position.z,
                        gn.female,
                        IsReadyToMate(cr, gn, tuning),
                        cr.species_id});
    }
    for (std::size_t i = 0; i < ents.size(); ++i) {
        const M& self = snap[i];
        if (!self.ready)
            continue;
        auto& cr = view.get<Comp::CreatureComponent>(self.e);
        if (cr.last_action == static_cast<int>(CreatureAction::Flee))
            continue;
        float best = tuning.mate_seek_radius, mx = 0.0f, mz = 0.0f;
        bool found = false;
        for (const M& o : snap) {
            if (o.e == self.e || !o.ready || o.female == self.female ||
                o.species_id != self.species_id)
                continue;
            const float dx = o.x - self.x, dz = o.z - self.z;
            const float d = dm::Sqrt(dx * dx + dz * dz);
            if (d < best) {
                best = d;
                mx = o.x;
                mz = o.z;
                found = true;
            }
        }
        if (!found)
            continue;
        const float dx = mx - self.x, dz = mz - self.z;
        const float d = dm::Sqrt(dx * dx + dz * dz);
        if (d > tuning.courtship_radius && d > 1.0e-5f) {
            const float inv = cr.move_speed / d;
            cr.wish_x = dx * inv;
            cr.wish_z = dz * inv;
        } else {
            cr.wish_x = 0.0f;
            cr.wish_z = 0.0f;
        }
    }
}

inline CreatureReproductionStats
RunMatingResolveOnTick(entt::registry& reg,
                       std::uint64_t tick,
                       std::uint64_t world_seed = 0,
                       const ReproductionTuning& tuning = {},
                       const CreatureSpeciesRegistry* species = nullptr) {
    CreatureReproductionStats stats;
    auto view = reg.view<Comp::CreatureComponent,
                         Comp::CreatureGenomeComponent,
                         Comp::TransformComponent>();
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    for (auto e : ents) {
        auto& gn = view.get<Comp::CreatureGenomeComponent>(e);
        if (gn.age_ticks < 0xFFFFFFFFu)
            ++gn.age_ticks;
        if (gn.reproduce_cooldown > 0)
            --gn.reproduce_cooldown;
    }
    struct Male {
        entt::entity e;
        float x, z;
        std::uint16_t species_id;
    };
    std::vector<Male> males;
    for (auto e : ents) {
        const auto& cr = view.get<Comp::CreatureComponent>(e);
        const auto& gn = view.get<Comp::CreatureGenomeComponent>(e);
        if (!gn.female && IsReadyToMate(cr, gn, tuning)) {
            const auto& tf = view.get<Comp::TransformComponent>(e);
            males.push_back({e, tf.position.x, tf.position.z, cr.species_id});
        }
    }
    struct Birth {
        CreatureGenome genome;
        float x, y, z;
        std::uint32_t generation;
        bool female;
        std::uint16_t species_id;
    };
    std::vector<Birth> births;
    std::vector<bool> male_used(males.size(), false);
    for (auto e : ents) {
        auto& cr = view.get<Comp::CreatureComponent>(e);
        auto& gn = view.get<Comp::CreatureGenomeComponent>(e);
        if (!gn.female || !IsReadyToMate(cr, gn, tuning)) {
            continue;
        }
        const auto& tf = view.get<Comp::TransformComponent>(e);
        int best = -1;
        float bestDist = tuning.courtship_radius;
        for (std::size_t m = 0; m < males.size(); ++m) {
            if (male_used[m] || males[m].species_id != cr.species_id)
                continue;
            const float dx = males[m].x - tf.position.x, dz = males[m].z - tf.position.z;
            const float d = dm::Sqrt(dx * dx + dz * dz);
            if (d < bestDist) {
                bestDist = d;
                best = static_cast<int>(m);
            }
        }
        if (best < 0) {
            gn.courting_ticks = 0;
            continue;
        }
        ++stats.courting;
        ++gn.courting_ticks;
        if (gn.courting_ticks < tuning.courtship_ticks)
            continue;
        const entt::entity maleE = males[static_cast<std::size_t>(best)].e;
        male_used[static_cast<std::size_t>(best)] = true;
        auto& mgn = view.get<Comp::CreatureGenomeComponent>(maleE);
        CreatureGenome fG;
        fG.move_speed = gn.move_speed;
        fG.vigilance = gn.vigilance;
        fG.hunger_threshold = gn.hunger_threshold;
        fG.size_scale = gn.size_scale;
        fG.vision_cos_half_fov = gn.vision_cos_half_fov;
        fG.vision_range = gn.vision_range;
        fG.hearing_range = gn.hearing_range;
        CreatureGenome mG;
        mG.move_speed = mgn.move_speed;
        mG.vigilance = mgn.vigilance;
        mG.hunger_threshold = mgn.hunger_threshold;
        mG.size_scale = mgn.size_scale;
        mG.vision_cos_half_fov = mgn.vision_cos_half_fov;
        mG.vision_range = mgn.vision_range;
        mG.hearing_range = mgn.hearing_range;
        luminumbra::core::DeterministicRng rng = luminumbra::core::DeterministicRng::seeded(
            kReproSeedOffset ^ world_seed,
            (static_cast<std::uint64_t>(entt::to_integral(e)) * 0x9E3779B97F4A7C15ull) ^
                static_cast<std::uint64_t>(entt::to_integral(maleE)),
            tick);
        const CreatureSpecies* species_definition =
            species ? species->Find(cr.species_id) : nullptr;
        const SpeciesGenomeRanges ranges =
            species_definition ? species_definition->genome_ranges : SpeciesGenomeRanges{};
        CreatureGenome childG = BreedOffspring(fG, mG, rng, ranges);
        const bool childFemale = (rng.next_u64() & 1ull) == 0ull;
        childG = BreedSensoryInto(childG, fG, mG, rng, ranges);
        const auto& mtf = view.get<Comp::TransformComponent>(maleE);
        Birth b;
        b.genome = childG;
        b.x = (tf.position.x + mtf.position.x) * 0.5f;
        b.y = (tf.position.y + mtf.position.y) * 0.5f;
        b.z = (tf.position.z + mtf.position.z) * 0.5f + tuning.spawn_radius;
        b.generation = (gn.generation > mgn.generation ? gn.generation : mgn.generation) + 1u;
        b.female = childFemale;
        b.species_id = cr.species_id;
        births.push_back(b);
        gn.courting_ticks = 0;
        gn.reproduce_cooldown = tuning.cooldown_ticks;
        mgn.reproduce_cooldown = tuning.cooldown_ticks;
    }
    for (const Birth& b : births) {
        const auto child = reg.create();
        auto& tf = reg.emplace<Comp::TransformComponent>(child);
        tf.position.x = b.x;
        tf.position.y = b.y;
        tf.position.z = b.z;
        tf.scale = ::Luminumbra::Vec3(b.genome.size_scale);
        auto& cr = reg.emplace<Comp::CreatureComponent>(child);
        cr.is_predator = false;
        cr.species_id = b.species_id;
        cr.hunger = 0.5f;
        cr.stamina = 1.0f;
        cr.move_speed = b.genome.move_speed;
        auto& gn = reg.emplace<Comp::CreatureGenomeComponent>(child);
        gn.move_speed = b.genome.move_speed;
        gn.vigilance = b.genome.vigilance;
        gn.hunger_threshold = b.genome.hunger_threshold;
        gn.size_scale = b.genome.size_scale;
        gn.vision_cos_half_fov = b.genome.vision_cos_half_fov;
        gn.vision_range = b.genome.vision_range;
        gn.hearing_range = b.genome.hearing_range;
        gn.age_ticks = 0;
        gn.reproduce_cooldown = tuning.cooldown_ticks;
        gn.generation = b.generation;
        gn.female = b.female;
        gn.courting_ticks = 0;
        auto& pc = reg.emplace<Comp::PerceptionComponent>(child);
        pc.vision_cos_half_fov = b.genome.vision_cos_half_fov;
        pc.vision_range = b.genome.vision_range;
        pc.ear.range = b.genome.hearing_range;
        ++stats.born;
    }
    return stats;
}

} // namespace LegacyCreatureTickOracle

namespace {
namespace C = Luminumbra::Components;
namespace F = luminumbra::foliage;
using Luminumbra::world::RegionConsumerGate;
using Luminumbra::world::RegionKey;
namespace W = Luminumbra::world;
namespace P = Luminumbra::Persistence;

entt::entity Plant(entt::registry& registry, float x) {
    const auto entity = registry.create();
    registry.emplace<C::PlantTag>(entity);
    registry.emplace<C::TransformComponent>(entity).position = {x, 0.0f, 0.0f};
    registry.emplace<C::PlantGenomeComponent>(entity).genes.fill(0.4f);
    registry.emplace<C::PlantGrowthComponent>(entity).stage =
        static_cast<std::uint8_t>(C::PlantStage::Fruiting);
    registry.emplace<C::PollinationTag>(entity);
    registry.emplace<C::PollinationComponent>(entity);
    registry.emplace<C::PlantHealthComponent>(entity);
    return entity;
}

std::string PlantBytes(const entt::registry& registry) {
    return Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
        F::BuildPlantEntitySnapshot(registry, true));
}

void AddCounters(entt::registry& registry, entt::entity entity) {
    auto& crop = registry.emplace<C::CropLifecycleComponent>(entity);
    crop.perennial = true;
    crop.lifespan_ticks = 2;
    crop.ripe_ticks = 1;
    auto& health = registry.get<C::PlantHealthComponent>(entity);
    health.state = static_cast<std::uint8_t>(C::PlantDiseaseState::Infected);
    health.infection = 800;
    auto& fire = registry.emplace<C::CombustibleComponent>(entity);
    fire.set_state(C::BurnState::Burning);
    fire.burn_ticks_remaining = 20;
    registry.emplace<C::GrazeableComponent>(entity).biomass = 0.8f;
    registry.emplace<C::MortalComponent>(entity).lifespan_ticks = 1;
}

// Return all telemetry as well as comparing the complete component projection.
nlohmann::json
StepConsumers(entt::registry& registry, const RegionConsumerGate& gate, bool explicit_gate = true) {
    const auto pollen = explicit_gate ? F::RunPollinationOnTick(registry, 100, {}, 0, 0.05f, gate)
                                      : F::RunPollinationOnTick(registry, 100);
    const auto disease = explicit_gate ? F::RunPlantDiseaseOnTick(registry, 100, 0, gate)
                                       : F::RunPlantDiseaseOnTick(registry, 100);
    const auto crops = explicit_gate ? F::RunCropLifecycleOnTick(registry, 100, 0, gate)
                                     : F::RunCropLifecycleOnTick(registry, 100);
    const auto fire = explicit_gate
                          ? luminumbra::sim::RunFireSpreadOnTick(registry, 100, {}, {}, gate)
                          : luminumbra::sim::RunFireSpreadOnTick(registry, 100);
    const auto grazing = explicit_gate
                             ? luminumbra::ai::RunWildlifeFoliageOnTick(registry, 100, {}, gate)
                             : luminumbra::ai::RunWildlifeFoliageOnTick(registry, 100);
    const auto lifespan = explicit_gate ? luminumbra::ai::RunLifespanOnTick(registry, 100, gate)
                                        : luminumbra::ai::RunLifespanOnTick(registry, 100);
    return {pollen.considered,
            pollen.donors,
            pollen.crossed,
            disease.considered,
            disease.infected,
            disease.new_infections,
            disease.recovered,
            disease.died,
            crops.ripening,
            crops.reseeded,
            crops.reset,
            crops.died,
            fire.considered,
            fire.ignited,
            fire.burnt_out,
            grazing.grazers,
            grazing.plants,
            grazing.grazed_plants,
            grazing.biomass_removed,
            grazing.biomass_regrown,
            lifespan.aged,
            lifespan.died};
}

TEST(RegionConsumerGate, BlockedOwnersRetainCountersAndCannotReseedOrReset) {
    entt::registry registry;
    for (const auto stage : {C::PlantStage::Juvenile, C::PlantStage::Fruiting}) {
        const auto entity = Plant(registry, 513.0f);
        AddCounters(registry, entity);
        registry.get<C::PlantGrowthComponent>(entity).stage = static_cast<std::uint8_t>(stage);
        registry.emplace<C::PlantRegionComponent>(
            entity, std::uint64_t{1} + entt::to_integral(entity), 1, 0, std::uint64_t{0});
    }
    const std::array<RegionKey, 1> due{{{0, 0}}};
    const RegionConsumerGate gate(registry, due);
    const auto before = PlantBytes(registry);
    const auto telemetry = StepConsumers(registry, gate);
    EXPECT_EQ(PlantBytes(registry), before);
    for (const auto& counter : telemetry) {
        EXPECT_EQ(counter, 0);
    }
    // The same roster takes all six mutation paths when admission is removed.
    const auto advanced = StepConsumers(registry, {});
    EXPECT_NE(PlantBytes(registry), before);
    EXPECT_GT(advanced.at(0).get<int>(), 0);
    EXPECT_GT(advanced.at(3).get<int>(), 0);
    EXPECT_EQ(advanced.at(9), 1);
    EXPECT_GT(advanced.at(12).get<int>(), 0);
    EXPECT_GT(advanced.at(19).get<float>(), 0.0f);
    EXPECT_GT(advanced.at(20).get<int>(), 0);
}

TEST(RegionConsumerGate, DiseaseAndPollenRequireBothSourceAndTargetAdmission) {
    for (const bool block_source : {false, true}) {
        SCOPED_TRACE(block_source);
        entt::registry registry;
        const auto source = Plant(registry, 512.5f);
        const auto target = Plant(registry, 511.5f);
        auto& health = registry.get<C::PlantHealthComponent>(source);
        health.state = static_cast<std::uint8_t>(C::PlantDiseaseState::Infected);
        health.infection = 1000;
        registry.get<C::PlantGrowthComponent>(target).stage =
            static_cast<std::uint8_t>(C::PlantStage::Juvenile);
        const std::array<RegionKey, 1> due{{{block_source ? 0 : 1, 0}}};
        const RegionConsumerGate gate(registry, due);
        const auto blocked = block_source ? source : target;
        const auto blocked_before = registry.get<C::PlantHealthComponent>(blocked);
        F::RunPlantDiseaseOnTick(registry, 1, 0, gate);
        F::RunPollinationOnTick(registry, 1, {}, 0, 0.05f, gate);
        EXPECT_EQ(registry.get<C::PlantHealthComponent>(target).infection, 0u);
        EXPECT_EQ(registry.get<C::PollinationComponent>(target).crosses, 0u);
        EXPECT_EQ(registry.get<C::PlantHealthComponent>(blocked).infection,
                  blocked_before.infection);
        EXPECT_EQ(registry.get<C::PlantHealthComponent>(blocked).infected_ticks,
                  blocked_before.infected_ticks);
        EXPECT_EQ(registry.get<C::PollinationComponent>(blocked).last_pollen_tick, 0u);
        F::RunPlantDiseaseOnTick(registry, 2);
        F::RunPollinationOnTick(registry, 2);
        EXPECT_GT(registry.get<C::PlantHealthComponent>(target).infection, 0u);
        EXPECT_EQ(registry.get<C::PollinationComponent>(target).crosses, 1u);
    }
}

TEST(RegionConsumerGate, EveryFireSourceAndTargetRequiresAdmission) {
    for (int source_kind = 0; source_kind < 3; ++source_kind) {
        for (const bool block_source : {false, true}) {
            SCOPED_TRACE(source_kind);
            SCOPED_TRACE(block_source);
            entt::registry registry;
            const auto source = Plant(registry, 512.25f);
            const auto target = Plant(registry, 511.75f);
            registry.emplace<C::CombustibleComponent>(target);
            std::vector<luminumbra::sim::FireIgnitionSource> external;
            if (source_kind == 0) {
                auto& burning = registry.emplace<C::CombustibleComponent>(source);
                burning.set_state(C::BurnState::Burning);
                burning.burn_ticks_remaining = 20;
            } else if (source_kind == 1) {
                registry.emplace<C::IgnitionSourceComponent>(source);
            } else {
                external.push_back({{512.25f, 0.0f}, 3.0f, 1.0f});
            }
            const std::array<RegionKey, 1> due{{{block_source ? 0 : 1, 0}}};
            const RegionConsumerGate gate(registry, due);
            luminumbra::sim::RunFireSpreadOnTick(registry, 1, {}, external, gate);
            EXPECT_EQ(registry.get<C::CombustibleComponent>(target).state(), C::BurnState::Unburnt);
            if (source_kind == 0 && block_source) {
                EXPECT_EQ(registry.get<C::CombustibleComponent>(source).burn_ticks_remaining, 20u);
            }
            luminumbra::sim::RunFireSpreadOnTick(registry, 2, {}, external);
            EXPECT_EQ(registry.get<C::CombustibleComponent>(target).state(), C::BurnState::Burning);
        }
    }
}

TEST(RegionConsumerGate, GrazingRequiresBothPlantAndCreatureAdmission) {
    for (const bool block_creature : {false, true}) {
        SCOPED_TRACE(block_creature);
        entt::registry registry;
        const auto plant = Plant(registry, 511.75f);
        registry.emplace<C::GrazeableComponent>(plant).biomass = 0.8f;
        const auto creature = registry.create();
        registry.emplace<C::CreatureComponent>(creature).hunger = 0.5f;
        registry.emplace<C::TransformComponent>(creature).position = {512.25f, 0.0f, 0.0f};
        const std::array<RegionKey, 1> due{{{block_creature ? 0 : 1, 0}}};
        const RegionConsumerGate gate(registry, due);
        const auto stats = luminumbra::ai::RunWildlifeFoliageOnTick(registry, 1, {}, gate);
        EXPECT_EQ(stats.grazed_plants, 0);
        EXPECT_EQ(registry.get<C::CreatureComponent>(creature).hunger, 0.5f);
        if (block_creature) {
            EXPECT_GT(registry.get<C::GrazeableComponent>(plant).biomass, 0.8f);
        } else {
            EXPECT_EQ(registry.get<C::GrazeableComponent>(plant).biomass, 0.8f);
        }
        luminumbra::ai::RunWildlifeFoliageOnTick(registry, 2);
        EXPECT_LT(registry.get<C::CreatureComponent>(creature).hunger, 0.5f);
        EXPECT_LT(registry.get<C::GrazeableComponent>(plant).biomass, 0.8f);
    }
}

TEST(RegionConsumerGate, DefaultGateMatchesLegacyBytesAndTelemetry) {
    entt::registry implicit, explicit_default, admitted, blocked;
    for (auto* registry : {&implicit, &explicit_default, &admitted, &blocked}) {
        AddCounters(*registry, Plant(*registry, 100.0f));
        Plant(*registry, 101.0f);
    }
    // Seed dispersal can cross z = 0, so the neighbouring owner is due as well.
    const std::array<RegionKey, 2> due{{{0, 0}, {0, -1}}};
    const RegionConsumerGate gate(admitted, due);
    const auto legacy = StepConsumers(implicit, {}, false);
    EXPECT_EQ(StepConsumers(explicit_default, {}), legacy);
    EXPECT_EQ(StepConsumers(admitted, gate), legacy);
    EXPECT_EQ(PlantBytes(explicit_default), PlantBytes(implicit));
    EXPECT_NE(PlantBytes(admitted), PlantBytes(implicit));
    const auto before = PlantBytes(blocked);
    EXPECT_NE(StepConsumers(blocked, RegionConsumerGate(blocked, {})), legacy);
    EXPECT_EQ(PlantBytes(blocked), before);
}

TEST(RegionConsumerGate, CreatureAndPlantMortalityShareAdmission) {
    for (const bool starvation : {false, true}) {
        for (int owner_case = 0; owner_case < 4; ++owner_case) {
            for (int mode = 0; mode < 3; ++mode) {
                SCOPED_TRACE(::testing::Message()
                             << starvation << ':' << owner_case << ':' << mode);
                entt::registry registry;
                const auto plant = Plant(registry, 513.0f);
                registry.emplace<C::MortalComponent>(plant).lifespan_ticks = 1u;
                const auto creature = registry.create();
                registry.emplace<C::CreatureComponent>(creature).hunger = 0.5f;
                registry.emplace<C::MortalComponent>(creature).lifespan_ticks =
                    starvation ? 10u : 2u;
                registry.emplace<C::TransformComponent>(creature).position = {513.0f, 0.0f, 0.0f};
                for (const auto entity : {plant, creature}) {
                    if (owner_case == 1) {
                        registry.remove<C::TransformComponent>(entity);
                    } else if (owner_case == 2) {
                        registry.get<C::TransformComponent>(entity).position.x =
                            std::numeric_limits<float>::infinity();
                    }
                }
                std::vector<RegionKey> due;
                if (owner_case != 3) {
                    due.push_back({0, 0});
                }
                const auto gate =
                    mode == 2 ? RegionConsumerGate(registry, due) : RegionConsumerGate{};
                for (const std::uint64_t tick :
                     {std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{3}}) {
                    registry.get<C::CreatureComponent>(creature).hunger =
                        starvation && tick >= 2u ? 1.0f : 0.5f;
                    const auto stats =
                        mode == 0 ? luminumbra::ai::RunLifespanOnTick(registry, tick)
                                  : luminumbra::ai::RunLifespanOnTick(registry, tick, gate);
                    const auto& mortal = registry.get<C::MortalComponent>(creature);
                    EXPECT_EQ(mortal.age_ticks, mode == 2 ? 0u : (tick == 1u ? 1u : 2u));
                    EXPECT_EQ(mortal.dead, mode != 2 && tick >= 2u ? 1u : 0u);
                    EXPECT_EQ(registry.get<C::CreatureComponent>(creature).eaten,
                              mode != 2 && tick >= 2u);
                    EXPECT_EQ(registry.get<C::MortalComponent>(plant).age_ticks,
                              mode == 2 ? 0u : 1u);
                    EXPECT_EQ(registry.get<C::MortalComponent>(plant).dead, mode == 2 ? 0u : 1u);
                    EXPECT_EQ(stats.aged, mode == 2 ? 0 : (tick == 1u ? 2 : (tick == 2u ? 1 : 0)));
                    EXPECT_EQ(stats.died, mode == 2 ? 0 : (tick == 1u ? 1 : (tick == 2u ? 1 : 0)));
                }
                EXPECT_EQ(gate.owner_visits(),
                          mode == 2 && (owner_case == 0 || owner_case == 3) ? 6u + due.size() : 0u);
            }
        }
    }
}

TEST(RegionConsumerGate, SoilConsumptionAndAbsorbedFollowPlantOwnerAdmission) {
    for (int mode = 0; mode < 8; ++mode) {
        SCOPED_TRACE(mode);
        entt::registry registry;
        F::SoilGrid soil(2, 1);
        const auto plant = Plant(registry, 0.5f);
        registry.emplace<C::SoilFeederComponent>(plant).absorbed = 7u;
        // Persistent ownership wins over the position inside the soil field.
        registry.emplace<C::PlantRegionComponent>(plant, std::uint64_t{1}, 1, 0, std::uint64_t{0});
        if (mode >= 5) {
            registry.remove<C::PlantRegionComponent>(plant);
        }
        if (mode == 5) {
            registry.get<C::TransformComponent>(plant).position.x =
                std::numeric_limits<float>::infinity();
        } else if (mode == 6) {
            registry.remove<C::TransformComponent>(plant);
        }
        std::vector<RegionKey> due;
        if (mode != 4) {
            due.push_back({mode == 2 ? 1 : 0, 0});
        }
        const auto gate = mode >= 2 ? RegionConsumerGate(registry, due) : RegionConsumerGate{};
        for (int step = 0; step < 2; ++step) {
            const auto stats =
                mode == 0 ? F::RunSoilNutrientOnTick(registry, soil, 0.0f, 0.0f, 1.0f)
                          : F::RunSoilNutrientOnTick(registry, soil, 0.0f, 0.0f, 1.0f, gate);
            const bool admitted = mode <= 2 || mode == 7;
            const auto steps = static_cast<std::uint32_t>(step + 1);
            EXPECT_EQ(stats.participants, admitted ? 1 : 0);
            EXPECT_EQ(stats.consumed, admitted ? 750 : 0);
            EXPECT_EQ(registry.get<C::SoilFeederComponent>(plant).absorbed,
                      7u + (admitted ? 750u * steps : 0u));
            EXPECT_EQ(soil.AtCell(0, 0),
                      F::kSoilBaseline -
                          (admitted ? (750 - F::kSoilRegenPerTick) * (step + 1) : 0));
            EXPECT_EQ(soil.AtCell(1, 0), F::kSoilBaseline);
            EXPECT_EQ(gate.owner_visits(),
                      mode >= 2 && mode != 5 && mode != 6 ? steps + due.size() : 0u);
        }
    }
}

TEST(RegionConsumerGate, DeniedSoilDemandCannotStealFromAdmittedPlantAndFieldStillRegenerates) {
    entt::registry registry;
    F::SoilGrid soil(2, 1, 100);
    const auto denied = Plant(registry, 0.5f);
    registry.emplace<C::SoilFeederComponent>(denied);
    registry.emplace<C::PlantRegionComponent>(denied, std::uint64_t{1}, 1, 0, std::uint64_t{0});
    const auto admitted = Plant(registry, 0.5f);
    registry.emplace<C::SoilFeederComponent>(admitted);
    const std::array<RegionKey, 1> due{{{0, 0}}};
    const RegionConsumerGate gate(registry, due);
    const auto stats = F::RunSoilNutrientOnTick(registry, soil, 0.0f, 0.0f, 1.0f, gate);
    EXPECT_EQ(stats.participants, 1);
    EXPECT_EQ(stats.consumed, 100);
    EXPECT_EQ(registry.get<C::SoilFeederComponent>(denied).absorbed, 0u);
    EXPECT_EQ(registry.get<C::SoilFeederComponent>(admitted).absorbed, 100u);
    EXPECT_EQ(soil.AtCell(0, 0), F::kSoilRegenPerTick);
    EXPECT_EQ(soil.AtCell(1, 0), 100 + F::kSoilRegenPerTick);
    EXPECT_EQ(gate.owner_visits(), 3u);
}

TEST(RegionConsumerGate, SessionSoilConsumptionUsesSealedPlantOwners) {
    for (const bool enabled : {false, true}) {
        for (const int owner : {0, 1}) {
            SCOPED_TRACE(::testing::Message() << enabled << ':' << owner);
            W::GameSession session;
            session.SetActiveRegionsEnabled(enabled);
            session.SetReplicatedSimulationAnchors({});
            session.PinActiveRegion({0, 0}, true);
            auto& registry = session.GetRegistry();
            const auto plant = Plant(registry, 0.0f);
            registry.emplace<C::PlantRegionComponent>(
                plant, std::uint64_t{1}, owner, 0, std::uint64_t{0});
            registry.emplace<C::SoilFeederComponent>(plant).absorbed = 7u;
            ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
            ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
            EXPECT_EQ(registry.get<C::SoilFeederComponent>(plant).absorbed,
                      enabled && owner == 1 ? 7u : 757u);
        }
    }
}

TEST(RegionConsumerGate, SessionUsesSealedDueOwnersAndFlagOffRetainsLegacyProgress) {
    for (const bool enabled : {false, true}) {
        SCOPED_TRACE(enabled);
        Luminumbra::world::GameSession session;
        session.SetActiveRegionsEnabled(enabled);
        session.SetReplicatedSimulationAnchors({});
        session.PinActiveRegion({0, 0}, true);
        auto& registry = session.GetRegistry();
        const auto entity = Plant(registry, 513.0f);
        AddCounters(registry, entity);
        Plant(registry, 514.0f);
        ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
        EXPECT_EQ(registry.get<C::CropLifecycleComponent>(entity).generations, enabled ? 0u : 1u);
        EXPECT_EQ(registry.get<C::PlantHealthComponent>(entity).infected_ticks, enabled ? 0u : 1u);
        EXPECT_EQ(registry.get<C::PollinationComponent>(entity).crosses, enabled ? 0u : 1u);
        EXPECT_EQ(registry.get<C::CombustibleComponent>(entity).burn_ticks_remaining,
                  enabled ? 20u : 19u);
        EXPECT_EQ(registry.get<C::GrazeableComponent>(entity).biomass > 0.8f, !enabled);
        EXPECT_EQ(registry.get<C::MortalComponent>(entity).age_ticks, enabled ? 0u : 1u);
    }
}

TEST(RegionConsumerGate, FrozenRegionDigestIncludesExactWaterDebtAndEmptyDebtKeepsLegacyBytes) {
    namespace W = Luminumbra::world;
    namespace P = Luminumbra::Persistence;
    constexpr RegionKey owner{8, 0};
    const W::RegionSchedulerConfig config{.work_limit = 1, .hold_ticks = 1, .reduced_shift = 1};
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    for (const std::uint64_t debt :
         {std::uint64_t{0}, std::uint64_t{1}, W::ActiveRegionLedger::kMaxWaterDebt}) {
        SCOPED_TRACE(debt);
        const Luminumbra::TestSupport::ScopedTempFile temporary("region_water_debt_", ".json");
        const auto directory = temporary.Path().parent_path();
        W::GameSession session;
        session.SetRootPath(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT).generic_string() + '/');
        session.SetSaveRootPath(directory);
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(true);
        session.SetAetherStateEnabled(false);
        session.SetRegionSchedulerConfig(config);
        ASSERT_TRUE(session.CreateTransientWorld("Regional water debt", "1337", "default"));
        W::WorldClock clock;
        W::ActiveRegionLedger ledger(config, clock);
        ledger.activate_owned(owner, clock);
        ledger.pin({0, 0}, true, clock);
        if (debt != 0) {
            ASSERT_TRUE(ledger.set_water_debts({{owner, debt}}));
        }
        nlohmann::json metadata{{"container_version", P::WorldSaveService::kContainerVersion}};
        clock.write_metadata(metadata);
        ASSERT_TRUE(P::WorldSaveService::save_metadata_and_active_regions(
            metadata.dump(), ledger, directory));
        ASSERT_TRUE(session.LoadWorldStateFrom(directory)) << session.GetWorldOpenError();
        session.SetReplicatedSimulationAnchors({});
        // One wake and two budget transitions suffice for this two-owner roster.
        for (int step = 0; step < 3; ++step) {
            session.RecordActiveRegionWork({{0, 0}, 2, 0, 0, 0});
            ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
            ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
        }
        const auto& record = session.GetActiveRegionLedger().records().at(owner);
        ASSERT_EQ(record.state, W::RegionState::Frozen);
        P::RegionDigestInputs inputs;
        inputs.require_index = false;
        if (debt != 0) {
            inputs.field_page_bytes = "|water_debt:";
            for (unsigned byte = 0; byte < 8; ++byte) {
                inputs.field_page_bytes.push_back(static_cast<char>((debt >> (8 * byte)) & 255));
            }
        }
        EXPECT_EQ(record.frozen_digest,
                  P::WorldSaveService::region_simulation_digest(directory, owner, inputs));
        if (debt != 0) {
            inputs.field_page_bytes.clear();
            EXPECT_NE(record.frozen_digest,
                      P::WorldSaveService::region_simulation_digest(directory, owner, inputs));
        }
    }
}

TEST(RegionConsumerGate, EmptyUnknownAndInvalidOwnersAreBlockedOnlyWithInstalledGate) {
    entt::registry registry;
    const auto entity = registry.create();
    const RegionConsumerGate empty(registry, {});
    EXPECT_FALSE(RegionConsumerGate{}.UsesPersistentPlantIdentity());
    EXPECT_TRUE(empty.UsesPersistentPlantIdentity());
    EXPECT_TRUE(RegionConsumerGate{}.Allows(entity));
    EXPECT_FALSE(empty.Allows(entity));
    registry.emplace<C::TransformComponent>(entity).position = {-0.5f, 0.0f, -0.5f};
    const std::array<RegionKey, 3> due{{{2, 1}, {-1, -1}, {2, 1}}};
    const RegionConsumerGate gate(registry, due);
    EXPECT_TRUE(gate.Allows(entity));
    EXPECT_FALSE(empty.Allows(entity));
    auto& position = registry.get<C::TransformComponent>(entity).position;
    position.x = 1024.0f;
    EXPECT_FALSE(gate.Allows(entity));
    position.x = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(gate.Allows(entity));
    EXPECT_TRUE(RegionConsumerGate{}.Allows(entity));
    registry.emplace<C::PlantRegionComponent>(entity, std::uint64_t{1}, -1, -1, std::uint64_t{0});
    EXPECT_TRUE(gate.Allows(entity));
    registry.get<C::PlantRegionComponent>(entity).owner_x = 0;
    EXPECT_FALSE(gate.Allows(entity));
}

TEST(RegionConsumerGate, AdmissionWorkScalesWithDueOwnersAndQueriedParticipants) {
    entt::registry registry;
    const auto participant = Plant(registry, 1.0f);
    for (int index = 0; index < 100; ++index) {
        Plant(registry, 1024.0f);
    }
    const std::array<RegionKey, 2> due{{{0, 0}, {1, 0}}};
    const RegionConsumerGate gate(registry, due);
    EXPECT_EQ(gate.owner_visits(), 0u);
    for (int query = 0; query < 10; ++query) {
        EXPECT_TRUE(gate.Allows(participant));
    }
    EXPECT_EQ(gate.owner_visits(), 12u);
}

TEST(RegionConsumerGate, MembershipIsLazyAndReusesCallerScratch) {
    entt::registry registry;
    const auto participant = Plant(registry, 1.0f);
    const std::array<RegionKey, 3> due{{{1, 0}, {0, 0}, {-1, 2}}};
    std::vector<std::uint64_t> scratch;
    {
        const RegionConsumerGate untouched(registry, due, scratch);
        EXPECT_TRUE(scratch.empty());
        EXPECT_EQ(untouched.owner_visits(), 0u);
    }
    EXPECT_TRUE(scratch.empty());
    const RegionConsumerGate first(registry, due, scratch);
    EXPECT_TRUE(first.Allows(participant));
    ASSERT_EQ(scratch.size(), 3u);
    EXPECT_TRUE(std::is_sorted(scratch.begin(), scratch.end()));
    const std::uint64_t* const storage = scratch.data();
    const std::array<RegionKey, 1> next{{{5, 5}}};
    const RegionConsumerGate second(registry, next, scratch);
    EXPECT_FALSE(second.Allows(participant));
    EXPECT_EQ(scratch.size(), 1u);
    EXPECT_EQ(scratch.data(), storage);
}

void Identify(entt::registry& registry, entt::entity entity, std::uint64_t id) {
    const auto owner =
        W::ActiveRegionLedger::region_at(registry.get<C::TransformComponent>(entity).position);
    registry.emplace<C::PlantRegionComponent>(entity, id, owner.x, owner.z, std::uint64_t{0});
}

void Ripen(entt::registry& registry, entt::entity entity, bool perennial) {
    auto& crop = registry.emplace<C::CropLifecycleComponent>(entity);
    crop.perennial = perennial;
    crop.lifespan_ticks = 2;
    crop.ripe_ticks = 1;
}

bool OpenLedger(W::GameSession& session,
                Luminumbra::JobSystem& jobs,
                const std::filesystem::path& directory,
                W::ActiveRegionLedger& ledger) {
    session.SetRootPath(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT).generic_string() + '/');
    session.SetSaveRootPath(directory);
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    session.SetAetherStateEnabled(false);
    if (!session.CreateTransientWorld("Plant identity", "1337", "default")) {
        return false;
    }
    std::filesystem::create_directories(directory);
    nlohmann::json metadata{{"container_version", P::WorldSaveService::kContainerVersion}};
    W::WorldClock{}.write_metadata(metadata);
    return P::WorldSaveService::save_metadata_and_active_regions(
               metadata.dump(), ledger, directory) &&
           session.LoadWorldStateFrom(directory);
}

TEST(RegionConsumerGate, IdentifiedAnnualAndPerennialReseedPassPersistentSnapshotAdmission) {
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    for (const bool perennial : {false, true}) {
        SCOPED_TRACE(perennial);
        const Luminumbra::TestSupport::ScopedTempFile temporary("plant_identity_", ".json");
        const auto directory = temporary.Path().parent_path();
        W::ActiveRegionLedger ledger;
        ledger.pin({0, 0}, true, W::WorldClock{});
        std::uint64_t parent_id = 0, reserved_id = 0;
        ASSERT_TRUE(ledger.allocate_persistent_id(parent_id));
        ASSERT_TRUE(ledger.allocate_persistent_id(reserved_id));
        W::GameSession session;
        ASSERT_TRUE(OpenLedger(session, jobs, directory / "input", ledger));
        auto& registry = session.GetRegistry();
        const auto parent = Plant(registry, 100.0f);
        registry.get<C::TransformComponent>(parent).position.z = 100.0f;
        Identify(registry, parent, parent_id);
        Ripen(registry, parent, perennial);
        ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
        const auto snapshot = F::BuildPlantEntitySnapshot(registry, true);
        std::string error;
        ASSERT_TRUE(
            F::ValidatePlantEntitySnapshot(snapshot, session.GetActiveRegionLedger(), {}, error))
            << error;
        EXPECT_EQ(snapshot.entities.size(), perennial ? 2u : 1u);
        bool found_child = false;
        for (const auto entity : registry.view<C::PlantRegionComponent>()) {
            const auto& identity = registry.get<C::PlantRegionComponent>(entity);
            const auto& growth = registry.get<C::PlantGrowthComponent>(entity);
            EXPECT_LE(growth.planted_tick, identity.region_stamp);
            EXPECT_LE(identity.region_stamp, growth.last_tick);
            if (identity.persistent_id != parent_id) {
                found_child = true;
                EXPECT_EQ(identity.persistent_id, reserved_id + 1u);
                EXPECT_EQ(identity.region_stamp, 1u);
                const auto owner = W::ActiveRegionLedger::region_at(
                    registry.get<C::TransformComponent>(entity).position);
                EXPECT_EQ(identity.owner_x, owner.x);
                EXPECT_EQ(identity.owner_z, owner.z);
            }
        }
        EXPECT_TRUE(found_child);
        EXPECT_TRUE(F::HasPersistentPlantIdentity(registry));
    }
}

TEST(RegionConsumerGate, PersistedRosterIgnoresHandleHistoryForCrossesAndBirths) {
    entt::registry source;
    for (const auto id : {std::uint64_t{30}, std::uint64_t{20}, std::uint64_t{10}}) {
        const auto entity = Plant(source, id == 30 ? 100.0f : (id == 10 ? 99.0f : 101.0f));
        source.get<C::TransformComponent>(entity).position.z = 100.0f;
        source.get<C::PlantGenomeComponent>(entity).genes.fill(id == 10 ? 0.2f : 0.8f);
        source.get<C::PlantGrowthComponent>(entity).growth_points = static_cast<std::uint32_t>(id);
        Identify(source, entity, id);
        Ripen(source, entity, true);
    }
    auto snapshot = F::BuildPlantEntitySnapshot(source, true);
    W::GameSession first, second;
    ASSERT_TRUE(F::ApplyPlantEntitySnapshot(first.GetRegistry(), snapshot));
    auto& other = second.GetRegistry();
    for (std::uint32_t index = 0; index < 7u; ++index) {
        (void)other.create();
    }
    const auto filler = other.create();
    other.destroy(filler);
    std::reverse(snapshot.entities.begin(), snapshot.entities.end());
    ASSERT_TRUE(F::ApplyPlantEntitySnapshot(other, snapshot));
    EXPECT_EQ(first.ComputePlantSubHash(), second.ComputePlantSubHash());
    for (auto* registry : {&first.GetRegistry(), &other}) {
        const std::array<RegionKey, 1> due{{{0, 0}}};
        EXPECT_EQ(F::RunPollinationOnTick(*registry,
                                          std::uint64_t{1},
                                          {},
                                          std::uint64_t{0},
                                          0.05f,
                                          RegionConsumerGate(*registry, due))
                      .crossed,
                  3);
        for (const auto entity : registry->view<C::PlantRegionComponent>()) {
            if (registry->get<C::PlantRegionComponent>(entity).persistent_id == 30u) {
                C::PlantGenomeComponent donor;
                donor.genes.fill(0.2f);
                EXPECT_EQ(registry->get<C::PollinationComponent>(entity).next_genome.genes,
                          F::PollinateCross(
                              registry->get<C::PlantGenomeComponent>(entity), donor, 30, 10, 1)
                              .genes);
            }
        }
    }
    EXPECT_EQ(PlantBytes(first.GetRegistry()), PlantBytes(other));
    for (auto* registry : {&first.GetRegistry(), &other}) {
        W::ActiveRegionLedger ledger;
        std::uint64_t id = 0;
        for (std::uint64_t reserved = 0; reserved < 30; ++reserved) {
            ASSERT_TRUE(ledger.allocate_persistent_id(id));
        }
        const auto allocate = [&ledger](std::uint64_t& child_id) {
            return ledger.allocate_persistent_id(child_id);
        };
        const std::array<RegionKey, 1> due{{{0, 0}}};
        EXPECT_EQ(F::RunCropLifecycleOnTick(*registry,
                                            std::uint64_t{1},
                                            std::uint64_t{0},
                                            RegionConsumerGate(*registry, due),
                                            allocate)
                      .reseeded,
                  3);
        EXPECT_TRUE(F::ValidatePlantEntitySnapshot(F::BuildPlantEntitySnapshot(*registry, true)));
    }
    EXPECT_EQ(PlantBytes(first.GetRegistry()), PlantBytes(other));
    EXPECT_EQ(first.ComputePlantSubHash(), second.ComputePlantSubHash());
}

TEST(RegionConsumerGate, BlockedBirthDestinationPreservesParentUntilDue) {
    for (const bool perennial : {false, true}) {
        entt::registry registry;
        const auto parent = Plant(registry, 511.5f);
        registry.get<C::TransformComponent>(parent).position.z = 100.0f;
        Identify(registry, parent, 1);
        Ripen(registry, parent, perennial);
        // Pick a deterministic outward dispersal, independent of registry handles.
        std::uint64_t tick = 1;
        for (; tick < 100; ++tick) {
            auto rng =
                luminumbra::core::DeterministicRng::seeded(F::kGerminationSeedOffset, 1, tick);
            const float angle = rng.next_unit() * 6.2831853f;
            const float radius = 1.0f + rng.next_unit() * 2.0f;
            if (radius * Luminumbra::DeterministicMath::Cos(angle) > 0.5f) {
                break;
            }
        }
        ASSERT_LT(tick, 100u);
        const std::array<RegionKey, 1> due{{{0, 0}}};
        const RegionConsumerGate gate(registry, due);
        std::uint32_t allocations = 0;
        const auto allocate = [&allocations](std::uint64_t& id) {
            ++allocations;
            id = 2;
            return true;
        };
        const auto before = PlantBytes(registry);
        const auto stats = F::RunCropLifecycleOnTick(registry, tick, 0, gate, allocate);
        EXPECT_EQ(PlantBytes(registry), before);
        EXPECT_EQ(allocations, 0u);
        EXPECT_EQ(gate.owner_visits(), 3u);
        EXPECT_EQ(stats.ripening, 0);
        EXPECT_EQ(stats.reseeded, 0);
        EXPECT_EQ(stats.reset, 0);
        EXPECT_EQ(stats.died, 0);
        const std::array<RegionKey, 2> both{{{0, 0}, {1, 0}}};
        EXPECT_EQ(F::RunCropLifecycleOnTick(
                      registry, tick + 1u, 0, RegionConsumerGate(registry, both), allocate)
                      .reseeded,
                  1);
        EXPECT_EQ(allocations, 1u);
    }
}

TEST(RegionConsumerGate, FailedIdentityAllocationLeavesEveryParentUntouched) {
    for (const bool perennial : {false, true}) {
        entt::registry registry;
        const auto parent = Plant(registry, 100.0f);
        Identify(registry, parent, 1);
        Ripen(registry, parent, perennial);
        const auto before = PlantBytes(registry);
        const auto refuse = [](std::uint64_t&) {
            return false;
        };
        const auto stats = F::RunCropLifecycleOnTick(registry, 1, 0, {}, refuse);
        EXPECT_EQ(PlantBytes(registry), before);
        EXPECT_EQ(stats.ripening, 0);
        EXPECT_EQ(stats.reseeded, 0);
        EXPECT_EQ(stats.reset, 0);
        EXPECT_EQ(stats.died, 0);
        const auto full = [](std::uint64_t&) -> bool {
            throw std::length_error("Active-region ledger file capacity reached");
        };
        EXPECT_THROW(F::RunCropLifecycleOnTick(registry, 1, 0, {}, full), std::length_error);
        EXPECT_EQ(PlantBytes(registry), before);
    }
}

TEST(RegionConsumerGate, SessionLatchesExhaustedSharedAllocatorWithoutReseeding) {
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    for (const bool perennial : {false, true}) {
        const Luminumbra::TestSupport::ScopedTempFile temporary("plant_exhaustion_", ".json");
        W::ActiveRegionLedger ledger;
        ledger.pin({0, 0}, true, W::WorldClock{});
        std::uint64_t id = 0;
        ASSERT_TRUE(ledger.allocate_persistent_id(id));
        auto encoded = ledger.encode();
        ASSERT_EQ(encoded.size(),
                  W::ActiveRegionLedger::kHeaderBytes + W::ActiveRegionLedger::kRecordBytes + 4u +
                      14u + 8u);
        // The sole extension is the allocator TLV; its u64 payload precedes the checksum.
        for (unsigned byte = 0; byte < 8; ++byte) {
            encoded[encoded.size() - 16u + byte] = static_cast<char>(
                (W::ActiveRegionLedger::kPersistentIdLimit >> (8u * byte)) & 255u);
        }
        std::uint64_t checksum = 14695981039346656037ull;
        for (std::size_t index = 0; index < encoded.size() - 8u; ++index) {
            checksum ^= static_cast<unsigned char>(encoded[index]);
            checksum *= 1099511628211ull;
        }
        for (unsigned byte = 0; byte < 8; ++byte) {
            encoded[encoded.size() - 8u + byte] =
                static_cast<char>((checksum >> (8u * byte)) & 255u);
        }
        std::string error;
        ASSERT_TRUE(W::ActiveRegionLedger::decode(encoded, ledger, error)) << error;
        W::GameSession session;
        ASSERT_TRUE(OpenLedger(session, jobs, temporary.Path().parent_path(), ledger));
        auto& registry = session.GetRegistry();
        const auto parent = Plant(registry, 100.0f);
        registry.get<C::TransformComponent>(parent).position.z = 100.0f;
        Identify(registry, parent, id);
        Ripen(registry, parent, perennial);
        ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
        const auto& fault = session.GetRegionSchedulerFault();
        if (fault.has_value()) {
            EXPECT_EQ(fault->message, "persistent id space exhausted");
        } else {
            FAIL() << "exhausted allocator did not latch a region fault";
        }
        ASSERT_TRUE(registry.valid(parent));
        EXPECT_EQ(registry.view<C::PlantTag>().size(), 1u);
        EXPECT_EQ(registry.get<C::CropLifecycleComponent>(parent).ripe_ticks, 1u);
        EXPECT_EQ(registry.get<C::CropLifecycleComponent>(parent).generations, 0u);
        EXPECT_EQ(registry.get<C::PlantGrowthComponent>(parent).stage,
                  static_cast<std::uint8_t>(C::PlantStage::Fruiting));
        EXPECT_EQ(session.GetActiveRegionLedger().next_persistent_id(),
                  W::ActiveRegionLedger::kPersistentIdLimit);
    }
}

TEST(RegionConsumerGate, GrowthStepsKeepPersistentStampCurrent) {
    entt::registry registry;
    const auto entity = Plant(registry, 100.0f);
    Identify(registry, entity, std::uint64_t{1});
    F::RunPlantGrowthSystemOnTick(
        registry, std::uint64_t{5}, [](const auto&) { return F::PlantEnvSample{}; });
    auto& identity = registry.get<C::PlantRegionComponent>(entity);
    EXPECT_EQ(identity.region_stamp, 5u);
    auto& growth = registry.get<C::PlantGrowthComponent>(entity);
    const auto& genome = registry.get<C::PlantGenomeComponent>(entity);
    F::AdvancePlantGrowthCoarse(growth, genome, {}, 0, 3, std::uint64_t{8}, &identity);
    EXPECT_EQ(identity.region_stamp, 8u);
    F::AdvancePlantGrowthCoarse(growth, genome, {}, 0, 0, std::uint64_t{9}, &identity);
    EXPECT_EQ(identity.region_stamp, 8u);
    EXPECT_EQ(growth.last_tick, 8u);
    const auto before = PlantBytes(registry);
    EXPECT_THROW(
        F::AdvancePlantGrowthCoarse(growth, genome, {}, 0, 36001, std::uint64_t{9}, &identity),
        std::invalid_argument);
    EXPECT_THROW(
        F::AdvancePlantGrowthCoarse(growth, genome, {}, 1001, 1, std::uint64_t{9}, &identity),
        std::invalid_argument);
    EXPECT_EQ(PlantBytes(registry), before);
    EXPECT_EQ(identity.region_stamp, 8u);
    EXPECT_EQ(growth.last_tick, 8u);
}

TEST(RegionConsumerGate, GrowthAdmissionPreservesDeniedStateAndSkipsSampling) {
    for (const bool persistent : {false, true}) {
        for (const bool empty_due : {false, true}) {
            entt::registry registry;
            const auto admitted = Plant(registry, 100.0f);
            const auto denied = Plant(registry, 513.0f);
            registry.emplace<C::PlantTag>(registry.create());
            for (int index = 0; index < 10; ++index) {
                (void)registry.create();
            }
            if (persistent) {
                Identify(registry, admitted, std::uint64_t{1});
                Identify(registry, denied, std::uint64_t{2});
                // Admission follows the saved owner even when the transform disagrees.
                registry.get<C::TransformComponent>(denied).position.x = 100.0f;
            }
            registry.get<C::PlantGrowthComponent>(denied).tended = 20;
            const auto before = PlantBytes(registry);
            std::uint32_t samples = 0;
            const auto sample = [&samples](const auto&) {
                ++samples;
                return F::PlantEnvSample{};
            };
            std::vector<RegionKey> due;
            if (!empty_due) {
                due.push_back({0, 0});
            }
            const RegionConsumerGate gate(registry, due);
            EXPECT_EQ(
                F::RunPlantGrowthSystemOnTick(registry, std::uint64_t{5}, sample, gate).updated,
                empty_due ? 0 : 1);
            EXPECT_EQ(samples, empty_due ? 0u : 1u);
            EXPECT_EQ(gate.owner_visits(), due.size() + 2u);
            const auto& growth = registry.get<C::PlantGrowthComponent>(denied);
            EXPECT_EQ(growth.last_tick, 0u);
            EXPECT_EQ(growth.growth_points, 0u);
            EXPECT_EQ(growth.stress_points, 0u);
            EXPECT_EQ(growth.tended, 20u);
            if (persistent) {
                EXPECT_EQ(registry.get<C::PlantRegionComponent>(denied).region_stamp, 0u);
            }
            if (empty_due) {
                EXPECT_EQ(PlantBytes(registry), before);
            }
            EXPECT_EQ(F::RunPlantGrowthSystemOnTick(registry, std::uint64_t{6}, sample).updated, 2);
        }
    }
}

TEST(RegionConsumerGate, EmptyAndInvalidGrowthParticipantsDoNotSampleEnvironment) {
    entt::registry registry;
    const std::array<RegionKey, 1> due{{{0, 0}}};
    const RegionConsumerGate gate(registry, due);
    std::uint32_t samples = 0;
    const auto sample = [&samples](const auto&) {
        ++samples;
        return F::PlantEnvSample{};
    };
    EXPECT_EQ(F::RunPlantGrowthSystemOnTick(registry, std::uint64_t{1}, sample, gate).updated, 0);
    EXPECT_EQ(gate.owner_visits(), 0u);
    const auto invalid = Plant(registry, std::numeric_limits<float>::infinity());
    EXPECT_EQ(F::RunPlantGrowthSystemOnTick(registry, std::uint64_t{2}, sample, gate).updated, 0);
    EXPECT_EQ(samples, 0u);
    EXPECT_EQ(gate.owner_visits(), 0u);
    EXPECT_EQ(registry.get<C::PlantGrowthComponent>(invalid).last_tick, 0u);
}

TEST(RegionConsumerGate, SessionGrowthUsesSealedOwnersAndDefaultGateKeepsFullRate) {
    for (const bool enabled : {false, true}) {
        W::GameSession session;
        session.SetActiveRegionsEnabled(enabled);
        session.SetReplicatedSimulationAnchors({});
        session.PinActiveRegion({0, 0}, true);
        auto& registry = session.GetRegistry();
        const auto plant = Plant(registry, 513.0f);
        Identify(registry, plant, std::uint64_t{1});
        ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
        const auto expected_tick = enabled ? std::uint64_t{0} : std::uint64_t{1};
        EXPECT_EQ(registry.get<C::PlantGrowthComponent>(plant).last_tick, expected_tick);
        EXPECT_EQ(registry.get<C::PlantRegionComponent>(plant).region_stamp, expected_tick);
    }
}

TEST(RegionConsumerGate, InactiveGatePinsRawPollinationIdsAndHandleOrder) {
    entt::registry registry;
    const auto receiver = Plant(registry, 100.0f);
    const auto donor = Plant(registry, 101.0f);
    registry.get<C::PlantGenomeComponent>(receiver).genes.fill(0.2f);
    registry.get<C::PlantGenomeComponent>(donor).genes.fill(0.8f);
    EXPECT_EQ(F::CollectPollinationParticipants(registry, {}),
              (std::vector<entt::entity>{receiver, donor}));
    const auto expected = F::PollinateCross(registry.get<C::PlantGenomeComponent>(receiver),
                                            registry.get<C::PlantGenomeComponent>(donor),
                                            static_cast<std::uint64_t>(entt::to_integral(receiver)),
                                            static_cast<std::uint64_t>(entt::to_integral(donor)),
                                            std::uint64_t{1});
    ASSERT_EQ(F::RunPollinationOnTick(registry, std::uint64_t{1}).crossed, 2);
    EXPECT_EQ(registry.get<C::PollinationComponent>(receiver).next_genome.genes, expected.genes);
    EXPECT_EQ(registry.get<C::PollinationComponent>(donor).next_genome.genes, expected.genes);
}

TEST(RegionConsumerGate, InactiveGatePinsRawGerminationAcrossGenerations) {
    entt::registry registry;
    auto parent = Plant(registry, 100.0f);
    Ripen(registry, parent, false);
    for (std::uint64_t tick = std::uint64_t{1}; tick <= std::uint64_t{3}; ++tick) {
        const auto position = registry.get<C::TransformComponent>(parent).position;
        const auto genome = registry.get<C::PlantGenomeComponent>(parent);
        auto rng = luminumbra::core::DeterministicRng::seeded(
            F::kGerminationSeedOffset, static_cast<std::uint64_t>(entt::to_integral(parent)), tick);
        const float angle = rng.next_unit() * 6.2831853f;
        const float radius = 1.0f + rng.next_unit() * 2.0f;
        auto& growth = registry.get<C::PlantGrowthComponent>(parent);
        growth.stage = static_cast<std::uint8_t>(C::PlantStage::Fruiting);
        registry.get<C::CropLifecycleComponent>(parent).ripe_ticks = 1;
        ASSERT_EQ(F::RunCropLifecycleOnTick(registry, tick, std::uint64_t{0}, {}).reseeded, 1);
        ASSERT_FALSE(registry.valid(parent));
        const auto children = F::CollectPollinationParticipants(registry, {});
        ASSERT_EQ(children.size(), 1u);
        parent = children.front();
        const auto& child = registry.get<C::TransformComponent>(parent).position;
        EXPECT_EQ(child.x, position.x + radius * Luminumbra::DeterministicMath::Cos(angle));
        EXPECT_EQ(child.z, position.z + radius * Luminumbra::DeterministicMath::Sin(angle));
        EXPECT_EQ(registry.get<C::PlantGenomeComponent>(parent).genes, genome.genes);
    }
}

TEST(RegionConsumerGate, MixedPlantIdsHaveDisjointRngDomainsAndSymmetricCrosses) {
    entt::registry registry;
    const auto persistent = Plant(registry, 100.0f);
    const auto legacy = Plant(registry, 101.0f);
    const auto handle = static_cast<std::uint64_t>(entt::to_integral(legacy));
    ASSERT_NE(handle, 0u);
    Identify(registry, persistent, handle);
    constexpr std::uint64_t legacy_tag = std::uint64_t{1} << 63;
    EXPECT_EQ(W::ActiveRegionLedger::kPersistentIdLimit, legacy_tag);
    EXPECT_EQ(F::PlantSimulationId(registry, persistent), handle);
    EXPECT_EQ(F::PlantSimulationId(registry, legacy), legacy_tag | handle);
    EXPECT_TRUE(F::PlantIdentityLess(registry, persistent, legacy));
    const std::array<RegionKey, 1> due{{{0, 0}}};
    const RegionConsumerGate gate(registry, due);
    EXPECT_EQ(F::CollectPollinationParticipants(registry, gate),
              (std::vector<entt::entity>{persistent, legacy}));
    registry.get<C::PlantGenomeComponent>(persistent).genes.fill(0.2f);
    registry.get<C::PlantGenomeComponent>(legacy).genes.fill(0.8f);
    const auto expected = F::PollinateCross(registry.get<C::PlantGenomeComponent>(persistent),
                                            registry.get<C::PlantGenomeComponent>(legacy),
                                            handle,
                                            legacy_tag | handle,
                                            std::uint64_t{1});
    ASSERT_EQ(F::RunPollinationOnTick(registry, std::uint64_t{1}, {}, std::uint64_t{0}, 0.05f, gate)
                  .crossed,
              2);
    EXPECT_EQ(registry.get<C::PollinationComponent>(persistent).next_genome.genes, expected.genes);
    EXPECT_EQ(registry.get<C::PollinationComponent>(legacy).next_genome.genes, expected.genes);
    registry.get<C::PlantRegionComponent>(persistent).persistent_id = legacy_tag - 1u;
    EXPECT_LT(F::PlantSimulationId(registry, persistent), F::PlantSimulationId(registry, legacy));
}

TEST(RegionConsumerGate, PollinationReservesParticipantBoundEvenWhenAdmissionIsSparse) {
    entt::registry registry;
    EXPECT_TRUE(F::CollectPollinationParticipants(registry, {}).empty());
    for (int index = 0; index < 8; ++index) {
        Plant(registry, 100.0f);
    }
    const auto blocked =
        F::CollectPollinationParticipants(registry, RegionConsumerGate(registry, {}));
    EXPECT_TRUE(blocked.empty());
    EXPECT_GE(blocked.capacity(), 8u);
    const auto admitted = F::CollectPollinationParticipants(registry, {});
    EXPECT_EQ(admitted.size(), 8u);
    EXPECT_GE(admitted.capacity(), 8u);
}

TEST(RegionConsumerGate, LegacyReseedKeepsHandleSeedAndNeverUsesIdentityAllocator) {
    entt::registry legacy, supplied;
    for (auto* registry : {&legacy, &supplied}) {
        const auto parent = Plant(*registry, 100.0f);
        Ripen(*registry, parent, true);
    }
    std::uint32_t allocations = 0;
    const auto allocate = [&allocations](std::uint64_t&) {
        ++allocations;
        return false;
    };
    EXPECT_EQ(F::RunCropLifecycleOnTick(legacy, std::uint64_t{1}).reseeded, 1);
    EXPECT_EQ(F::RunCropLifecycleOnTick(supplied, std::uint64_t{1}, std::uint64_t{0}, {}, allocate)
                  .reseeded,
              1);
    EXPECT_EQ(allocations, 0u);
    const auto bytes = [](const entt::registry& registry) {
        return Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
            F::BuildPlantEntitySnapshot(registry));
    };
    EXPECT_EQ(bytes(legacy), bytes(supplied));
    auto rng = luminumbra::core::DeterministicRng::seeded(
        F::kGerminationSeedOffset,
        static_cast<std::uint64_t>(entt::to_integral(entt::entity{0})),
        std::uint64_t{1});
    const float angle = rng.next_unit() * 6.2831853f;
    const float radius = 1.0f + rng.next_unit() * 2.0f;
    for (const auto entity : legacy.view<C::PlantGrowthComponent>()) {
        EXPECT_FALSE(legacy.all_of<C::PlantRegionComponent>(entity));
        if (legacy.get<C::PlantGrowthComponent>(entity).planted_tick == 1u) {
            const auto& position = legacy.get<C::TransformComponent>(entity).position;
            EXPECT_EQ(position.x, 100.0f + radius * Luminumbra::DeterministicMath::Cos(angle));
            EXPECT_EQ(position.z, radius * Luminumbra::DeterministicMath::Sin(angle));
        }
    }
}
TEST(RegionConsumerGate, IdentityOrderingIsStrictForMixedZeroAndDuplicateIds) {
    entt::registry registry;
    std::array<entt::entity, 5> entities{};
    for (auto& entity : entities) {
        entity = Plant(registry, 100.0f);
    }
    Identify(registry, entities[1], std::uint64_t{8});
    Identify(registry, entities[2], std::uint64_t{0});
    Identify(registry, entities[3], std::uint64_t{3});
    Identify(registry, entities[4], std::uint64_t{3});
    EXPECT_EQ(F::CollectPollinationParticipants(registry, {}),
              (std::vector<entt::entity>(entities.begin(), entities.end())));
    const std::array expected{entities[3], entities[4], entities[1], entities[0], entities[2]};
    std::sort(entities.begin(), entities.end(), [&registry](auto a, auto b) {
        return F::PlantIdentityLess(registry, a, b);
    });
    EXPECT_EQ(entities, expected);
    for (std::size_t a = 0; a < entities.size(); ++a) {
        for (std::size_t b = 0; b < entities.size(); ++b) {
            EXPECT_EQ(F::PlantIdentityLess(registry, entities[a], entities[b]), a < b);
        }
    }
    EXPECT_EQ(F::PlantSimulationId(registry, entities[0]), 3u);
    constexpr std::uint64_t legacy_tag = std::uint64_t{1} << 63;
    EXPECT_EQ(F::PlantSimulationId(registry, entities[3]),
              legacy_tag | static_cast<std::uint64_t>(entt::to_integral(entities[3])));
    EXPECT_EQ(F::PlantSimulationId(registry, entities[4]),
              legacy_tag | static_cast<std::uint64_t>(entt::to_integral(entities[4])));
}
} // namespace

namespace {
namespace AI = luminumbra::ai;

entt::entity Creature(entt::registry& registry, float x, bool female = false) {
    const auto entity = registry.create();
    registry.emplace<C::TransformComponent>(entity).position = {x, 0.0f, 10.0f};
    registry.emplace<C::CreatureComponent>(entity).hunger = 0.2f;
    auto& genome = registry.emplace<C::CreatureGenomeComponent>(entity);
    genome.age_ticks = 100u;
    genome.female = female;
    genome.courting_ticks = AI::kCourtshipTicks - 1u;
    registry.emplace<C::MortalComponent>(entity).lifespan_ticks = 10000u;
    registry.emplace<C::PerceptionComponent>(entity);
    registry.emplace<C::AwarenessComponent>(entity).awareness.meter = 0.4f;
    return entity;
}

std::string CreatureBytes(const entt::registry& registry, entt::entity entity) {
    const auto& tf = registry.get<C::TransformComponent>(entity);
    const auto& cr = registry.get<C::CreatureComponent>(entity);
    nlohmann::json bytes = {tf.position.x,
                            tf.position.y,
                            tf.position.z,
                            cr.is_predator,
                            cr.species_id,
                            cr.hunger,
                            cr.stamina,
                            cr.energy,
                            cr.move_speed,
                            cr.last_action,
                            cr.wish_x,
                            cr.wish_z,
                            cr.eaten};
    if (const auto* gn = registry.try_get<C::CreatureGenomeComponent>(entity)) {
        bytes.push_back({gn->move_speed,
                         gn->vigilance,
                         gn->hunger_threshold,
                         gn->size_scale,
                         gn->vision_cos_half_fov,
                         gn->vision_range,
                         gn->hearing_range,
                         gn->age_ticks,
                         gn->reproduce_cooldown,
                         gn->generation,
                         gn->female,
                         gn->courting_ticks});
    }
    if (const auto* mortal = registry.try_get<C::MortalComponent>(entity)) {
        bytes.push_back({mortal->age_ticks, mortal->lifespan_ticks, mortal->dead});
    }
    if (const auto* aw = registry.try_get<C::AwarenessComponent>(entity)) {
        const auto& a = aw->awareness;
        bytes.push_back({a.meter,
                         static_cast<int>(a.state),
                         a.has_last_known,
                         a.last_known_x,
                         a.last_known_z,
                         a.time_since_signal});
    }
    return bytes.dump();
}

void CoreCreatureTick(entt::registry& registry,
                      std::uint64_t tick,
                      const RegionConsumerGate& gate = {},
                      const AI::CreatureTickAdmission* admission = nullptr,
                      bool legacy = false) {
    constexpr float dt = 1.0f / 30.0f;
    AI::RunPerceptionSystemOnTick(registry, dt, admission);
    if (legacy) {
        LegacyCreatureTickOracle::RunCreatureBrainSystemOnTick(registry, dt);
        LegacyCreatureTickOracle::RunMateSeekingOnTick(registry);
        LegacyCreatureTickOracle::RunMatingResolveOnTick(registry, tick, 12345u);
    } else {
        AI::RunCreatureBrainSystemOnTick(
            registry, dt, {}, nullptr, 0, 0, 0, true, nullptr, admission);
        AI::RunMateSeekingOnTick(registry, {}, admission);
        AI::RunMatingResolveOnTick(registry, tick, 12345u, {}, nullptr, admission);
    }
    AI::RunLifespanOnTick(registry, tick, gate, admission);
}

TEST(RegionConsumerGate, SessionFreezesCoreCreatureStateAndResumesWithItsPlantOwner) {
    for (const bool active : {false, true}) {
        W::GameSession session;
        session.SetActiveRegionsEnabled(active);
        session.SetReplicatedSimulationAnchors({});
        session.PinActiveRegion({0, 0}, true);
        auto& registry = session.GetRegistry();
        const auto creature = Creature(registry, 513.0f, true);
        Creature(registry, 514.0f);
        const auto plant = Plant(registry, 513.0f);
        registry.emplace<C::MortalComponent>(plant).lifespan_ticks = 10000u;
        const auto before = CreatureBytes(registry, creature);
        ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
        EXPECT_EQ(CreatureBytes(registry, creature) == before, active);
        EXPECT_EQ(registry.get<C::MortalComponent>(plant).age_ticks, active ? 0u : 1u);
        EXPECT_EQ(registry.view<C::CreatureComponent>().size(), active ? 2u : 3u);
        session.PinActiveRegion({1, 0}, true);
        ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
        EXPECT_NE(CreatureBytes(registry, creature), before);
        EXPECT_EQ(registry.get<C::MortalComponent>(creature).age_ticks, active ? 1u : 2u);
        EXPECT_EQ(registry.get<C::MortalComponent>(plant).age_ticks, active ? 1u : 2u);
    }
}

TEST(RegionConsumerGate, DeniedParentsCannotConsumeBirthIdsOrPerturbAdmittedOffspring) {
    entt::registry actual, reference;
    for (auto* registry : {&actual, &reference}) {
        Creature(*registry, 513.0f, true);
        Creature(*registry, 514.0f);
        Creature(*registry, 10.0f, true);
        Creature(*registry, 11.0f);
    }
    const auto blocked = static_cast<entt::entity>(0u);
    const auto before = CreatureBytes(actual, blocked);
    reference.remove<C::CreatureComponent>(blocked);
    reference.remove<C::CreatureComponent>(static_cast<entt::entity>(1u));
    const std::array<RegionKey, 1> due{{{0, 0}}};
    const RegionConsumerGate gate(actual, due);
    AI::CreatureTickAdmission admission;
    admission.Capture(actual, gate);
    CoreCreatureTick(actual, 1u, gate, &admission);
    CoreCreatureTick(reference, 1u);
    EXPECT_EQ(CreatureBytes(actual, blocked), before);
    ASSERT_EQ(actual.view<C::CreatureComponent>().size(), 5u);
    for (const auto entity : reference.view<C::CreatureComponent>()) {
        EXPECT_EQ(CreatureBytes(actual, entity), CreatureBytes(reference, entity));
    }
}

TEST(RegionConsumerGate, CrossingUsesStartPositionThroughReproductionAndMortality) {
    for (const bool east : {false, true}) {
        entt::registry registry;
        if (east) {
            static_cast<void>(registry.create());
        }
        const auto creature = Creature(registry, east ? 511.999f : 512.001f);
        registry.get<C::CreatureComponent>(creature).is_predator = true;
        const std::array<RegionKey, 1> due{{{east ? 0 : 1, 0}}};
        const RegionConsumerGate gate(registry, due);
        AI::CreatureTickAdmission admission;
        admission.Capture(registry, gate);
        CoreCreatureTick(registry, 1u, gate, &admission);
        EXPECT_FALSE(gate.Allows(creature));
        EXPECT_EQ(registry.get<C::CreatureGenomeComponent>(creature).age_ticks, 101u);
        EXPECT_EQ(registry.get<C::MortalComponent>(creature).age_ticks, 1u);
        const auto after_crossing = CreatureBytes(registry, creature);
        admission.Capture(registry, gate);
        CoreCreatureTick(registry, 2u, gate, &admission);
        EXPECT_EQ(CreatureBytes(registry, creature), after_crossing);
    }
}

TEST(RegionConsumerGate, EmptyAndInvalidCreatureOwnersAreDeniedBeforeCoreWork) {
    entt::registry registry;
    AI::CreatureTickAdmission admission;
    admission.Capture(registry, RegionConsumerGate(registry, {}));
    EXPECT_EQ(admission.owner_visits(), 0u);
    const auto missing = Creature(registry, 1.0f);
    registry.remove<C::TransformComponent>(missing);
    const auto invalid = Creature(registry, std::numeric_limits<float>::infinity());
    const auto valid = Creature(registry, -1.0f);
    const auto other = registry.create();
    for (const bool empty : {false, true}) {
        const std::vector<RegionKey> due =
            empty ? std::vector<RegionKey>{} : std::vector<RegionKey>{{-1, 0}, {-1, 0}};
        const RegionConsumerGate gate(registry, due);
        admission.Capture(registry, gate);
        EXPECT_EQ(admission.owner_visits(), 3u);
        EXPECT_FALSE(admission.Allows(missing));
        EXPECT_FALSE(admission.Allows(invalid));
        EXPECT_EQ(admission.Allows(valid), !empty);
        EXPECT_TRUE(admission.Allows(other));
        CoreCreatureTick(registry, 1u, gate, &admission);
        EXPECT_EQ(registry.get<C::MortalComponent>(missing).age_ticks, 0u);
        EXPECT_EQ(registry.get<C::MortalComponent>(invalid).age_ticks, 0u);
    }
}

TEST(RegionConsumerGate, InactiveCreaturePipelineMatchesIndependentFullRateOracle) {
    entt::registry actual, reference;
    for (auto* registry : {&actual, &reference}) {
        Creature(*registry, 10.0f, true);
        Creature(*registry, 11.0f);
        const auto predator = Creature(*registry, 18.0f);
        registry->get<C::CreatureComponent>(predator).is_predator = true;
        registry->get<C::CreatureGenomeComponent>(predator).vision_range = 0.0f;
        registry->get<C::CreatureGenomeComponent>(predator).hearing_range = 0.0f;
    }
    for (std::uint64_t tick = 1u; tick <= 32u; ++tick) {
        CoreCreatureTick(actual, tick);
        CoreCreatureTick(reference, tick, {}, nullptr, true);
        ASSERT_EQ(actual.view<C::CreatureComponent>().size(),
                  reference.view<C::CreatureComponent>().size());
        for (const auto entity : reference.view<C::CreatureComponent>()) {
            EXPECT_EQ(CreatureBytes(actual, entity), CreatureBytes(reference, entity));
        }
    }
    EXPECT_GT(actual.view<C::CreatureComponent>().size(), 3u);
}
} // namespace

namespace {
TEST(RegionConsumerGate, AdmittedPredatorCannotChangeDeniedPreyAcrossBoundary) {
    entt::registry registry;
    const auto predator = Creature(registry, 511.9f);
    registry.get<C::CreatureComponent>(predator).is_predator = true;
    const auto prey = Creature(registry, 512.1f);
    const auto before = CreatureBytes(registry, prey);
    const std::array<RegionKey, 1> due{{{0, 0}}};
    const RegionConsumerGate gate(registry, due);
    AI::CreatureTickAdmission admission;
    admission.Capture(registry, gate);
    CoreCreatureTick(registry, 1u, gate, &admission);
    EXPECT_EQ(CreatureBytes(registry, prey), before);
    EXPECT_FALSE(registry.get<C::CreatureComponent>(prey).eaten);
    EXPECT_GT(registry.get<C::MortalComponent>(predator).age_ticks, 0u);
}
} // namespace
